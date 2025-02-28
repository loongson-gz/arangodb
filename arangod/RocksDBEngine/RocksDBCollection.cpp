////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBCollection.h"

#include "Aql/PlanCache.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Cache/CacheManagerFeature.h"
#include "Cache/Common.h"
#include "Cache/Manager.h"
#include "Cache/TransactionalCache.h"
#include "Cluster/ClusterMethods.h"
#include "Indexes/Index.h"
#include "Indexes/IndexIterator.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBBuilderIndex.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBComparator.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBIterators.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBMethods.h"
#include "RocksDBEngine/RocksDBPrimaryIndex.h"
#include "RocksDBEngine/RocksDBSettingsManager.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/OperationOptions.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LocalDocumentId.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ManagedDocumentResult.h"
#include "VocBase/ticks.h"
#include "VocBase/voc-types.h"

#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;

RocksDBCollection::RocksDBCollection(LogicalCollection& collection,
                                     arangodb::velocypack::Slice const& info)
    : RocksDBMetaCollection(collection, info),
      _primaryIndex(nullptr),
      _cache(nullptr),
      _cacheEnabled(
          !collection.system() &&
          basics::VelocyPackHelper::getBooleanValue(info, "cacheEnabled", false) &&
          CacheManagerFeature::MANAGER != nullptr),
      _numIndexCreations(0) {
  TRI_ASSERT(_logicalCollection.isAStub() || _objectId != 0);
  if (_cacheEnabled) {
    createCache();
  }
}

RocksDBCollection::RocksDBCollection(LogicalCollection& collection,
                                     PhysicalCollection const* physical)
    : RocksDBMetaCollection(collection, VPackSlice::emptyObjectSlice()),
      _primaryIndex(nullptr),
      _cache(nullptr),
      _cacheEnabled(static_cast<RocksDBCollection const*>(physical)->_cacheEnabled &&
                    CacheManagerFeature::MANAGER != nullptr),
      _numIndexCreations(0) {
  if (_cacheEnabled) {
    createCache();
  }
}

RocksDBCollection::~RocksDBCollection() {
  if (useCache()) {
    try {
      destroyCache();
    } catch (...) {
    }
  }
}

Result RocksDBCollection::updateProperties(VPackSlice const& slice, bool doSync) {
  auto isSys = _logicalCollection.system();

  _cacheEnabled =
      !isSys &&
      basics::VelocyPackHelper::getBooleanValue(slice, "cacheEnabled", _cacheEnabled) &&
      CacheManagerFeature::MANAGER != nullptr;
  primaryIndex()->setCacheEnabled(_cacheEnabled);

  if (_cacheEnabled) {
    createCache();
    primaryIndex()->createCache();
  } else {
    // will do nothing if cache is not present
    destroyCache();
    primaryIndex()->destroyCache();
    TRI_ASSERT(_cache.get() == nullptr);
  }

  // nothing else to do
  return TRI_ERROR_NO_ERROR;
}

PhysicalCollection* RocksDBCollection::clone(LogicalCollection& logical) const {
  return new RocksDBCollection(logical, this);
}

/// @brief export properties
void RocksDBCollection::getPropertiesVPack(velocypack::Builder& result) const {
  TRI_ASSERT(result.isOpenObject());
  result.add("objectId", VPackValue(std::to_string(_objectId)));
  result.add("cacheEnabled", VPackValue(_cacheEnabled));
  TRI_ASSERT(result.isOpenObject());
}

/// @brief closes an open collection
int RocksDBCollection::close() {
  READ_LOCKER(guard, _indexesLock);
  for (auto it : _indexes) {
    it->unload();
  }
  return TRI_ERROR_NO_ERROR;
}

void RocksDBCollection::load() {
  if (_cacheEnabled) {
    createCache();
    if (_cache) {
      uint64_t numDocs = _meta.numberDocuments();
      if (numDocs > 0) {
        _cache->sizeHint(static_cast<uint64_t>(0.3 * numDocs));
      }
    }
  }
  READ_LOCKER(guard, _indexesLock);
  for (auto it : _indexes) {
    it->load();
  }
}

void RocksDBCollection::unload() {
  WRITE_LOCKER(guard, _exclusiveLock);
  if (useCache()) {
    destroyCache();
    TRI_ASSERT(_cache.get() == nullptr);
  }
  READ_LOCKER(indexGuard, _indexesLock);
  for (auto it : _indexes) {
    it->unload();
  }
}

/// return bounds for all documents
RocksDBKeyBounds RocksDBCollection::bounds() const  {
  return RocksDBKeyBounds::CollectionDocuments(objectId());
}

void RocksDBCollection::prepareIndexes(arangodb::velocypack::Slice indexesSlice) {
  TRI_ASSERT(indexesSlice.isArray());

  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  std::vector<std::shared_ptr<Index>> indexes;
  {
    READ_LOCKER(guard, _indexesLock);  // link creation needs read-lock too
    if (indexesSlice.length() == 0 && _indexes.empty()) {
      engine->indexFactory().fillSystemIndexes(_logicalCollection, indexes);
    } else {
      engine->indexFactory().prepareIndexes(_logicalCollection, indexesSlice, indexes);
    }
  }

  WRITE_LOCKER(guard, _indexesLock);
  TRI_ASSERT(_indexes.empty());
  for (std::shared_ptr<Index>& idx : indexes) {
    TRI_ASSERT(idx != nullptr);
    auto const id = idx->id();
    for (auto const& it : _indexes) {
      TRI_ASSERT(it != nullptr);
      if (it->id() == id) {  // index is there twice
        idx.reset();
        break;
      }
    }

    if (idx) {
      TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(id));
      _indexes.emplace(idx);
      if (idx->type() == Index::TRI_IDX_TYPE_PRIMARY_INDEX) {
        TRI_ASSERT(idx->id() == 0);
        _primaryIndex = static_cast<RocksDBPrimaryIndex*>(idx.get());
      }
    }
  }

  auto it = _indexes.cbegin();
  if ((*it)->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX ||
      (TRI_COL_TYPE_EDGE == _logicalCollection.type() &&
       (_indexes.size() < 3 ||
        ((*++it)->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX ||
         (*++it)->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX)))) {
    std::string msg =
        "got invalid indexes for collection '" + _logicalCollection.name() + "'";
    LOG_TOPIC("0ef34", ERR, arangodb::Logger::ENGINES) << msg;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    for (auto it : _indexes) {
      LOG_TOPIC("19e0b", ERR, arangodb::Logger::ENGINES) << "- " << it->context();
    }
#endif
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, msg);
  }

  TRI_ASSERT(!_indexes.empty());
}

std::shared_ptr<Index> RocksDBCollection::createIndex(VPackSlice const& info,
                                                      bool restore, bool& created) {
  TRI_ASSERT(info.isObject());

  // Step 0. Lock all the things
  TRI_vocbase_t& vocbase = _logicalCollection.vocbase();
  TRI_vocbase_col_status_e status;
  Result res = vocbase.useCollection(&_logicalCollection, status);

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }
  _numIndexCreations.fetch_add(1, std::memory_order_release);
  auto colGuard = scopeGuard([&] {
    vocbase.releaseCollection(&_logicalCollection);
    _numIndexCreations.fetch_sub(1, std::memory_order_release);
  });

  RocksDBBuilderIndex::Locker locker(this);
  if (!locker.lock()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_LOCK_TIMEOUT);
  }

  std::shared_ptr<Index> idx;
  {  // Step 1. Check for matching index
    READ_LOCKER(guard, _indexesLock);
    if ((idx = findIndex(info, _indexes)) != nullptr) {
      // We already have this index.
      if (idx->type() == arangodb::Index::TRI_IDX_TYPE_TTL_INDEX) {
        // special handling for TTL indexes
        // if there is exactly the same index present, we return it
        if (idx->matchesDefinition(info)) {
          created = false;
          return idx;
        }
        // if there is another TTL index already, we make things abort here
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_BAD_PARAMETER,
            "there can only be one ttl index per collection");
      }

      created = false;
      return idx;
    }
  }

  RocksDBEngine* engine = static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);

  // Step 2. We are sure that we do not have an index of this type.
  // We also hold the lock. Create it
  bool const generateKey = !restore;
  try {
    idx = engine->indexFactory().prepareIndexFromSlice(info, generateKey,
                                                       _logicalCollection, false);
  } catch (std::exception const& ex) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_INDEX_CREATION_FAILED, ex.what());
  }

  // we cannot persist primary or edge indexes
  TRI_ASSERT(idx->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);
  TRI_ASSERT(idx->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX);

  {
    READ_LOCKER(guard, _indexesLock);
    for (auto const& other : _indexes) {  // conflicting index exists
      if (other->id() == idx->id() || other->name() == idx->name()) {
        // definition shares an identifier with an existing index with a
        // different definition
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        VPackBuilder builder;
        other->toVelocyPack(builder, static_cast<std::underlying_type<Index::Serialize>::type>(
                                         Index::Serialize::Basics));
        LOG_TOPIC("29d1c", WARN, Logger::ENGINES)
            << "attempted to create index '" << info.toJson()
            << "' but found conflicting index '" << builder.slice().toJson() << "'";
#endif
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER,
                                       "duplicate value for `" + StaticStrings::IndexId +
                                           "` or `" + StaticStrings::IndexName + "`");
      }
    }
  }

  do {

    // Step 3. add index to collection entry (for removal after a crash)
    auto buildIdx =
    std::make_shared<RocksDBBuilderIndex>(std::static_pointer_cast<RocksDBIndex>(idx));
    if (!engine->inRecovery()) {  // manually modify collection entry, other
      // methods need lock
      RocksDBKey key;             // read collection info from database
      key.constructCollection(_logicalCollection.vocbase().id(),
                              _logicalCollection.id());
      rocksdb::PinnableSlice ps;
      rocksdb::Status s = engine->db()->Get(rocksdb::ReadOptions(),
                                            RocksDBColumnFamily::definitions(),
                                            key.string(), &ps);
      if (!s.ok()) {
        res.reset(rocksutils::convertStatus(s));
        break;
      }
      
      VPackBuilder builder;
      builder.openObject();
      for (auto pair : VPackObjectIterator(RocksDBValue::data(ps))) {
        if (pair.key.isEqualString("indexes")) {  // append new index
          VPackArrayBuilder arrGuard(&builder, "indexes");
          builder.add(VPackArrayIterator(pair.value));
          buildIdx->toVelocyPack(builder, Index::makeFlags(Index::Serialize::Internals));
          continue;
        }
        builder.add(pair.key);
        builder.add(pair.value);
      }
      builder.close();
      res = engine->writeCreateCollectionMarker(_logicalCollection.vocbase().id(),
                                                _logicalCollection.id(), builder.slice(),
                                                RocksDBLogValue::Empty());
      if (res.fail()) {
        break;
      }
    }

    // Step 4. fill index
    const bool inBackground =
    basics::VelocyPackHelper::getBooleanValue(info, StaticStrings::IndexInBackground, false);
    if (inBackground) {  // allow concurrent inserts into index
      _indexes.emplace(buildIdx);
      res = buildIdx->fillIndexBackground(locker);
    } else {
      res = buildIdx->fillIndexForeground();
    }
    if (res.fail()) {
      break;
    }
    locker.lock(); // always lock to avoid inconsistencies

    // Step 5. register in index list
    WRITE_LOCKER(guard, _indexesLock);
    if (inBackground) {  // swap in actual index
      for (auto& it : _indexes) {
        if (it->id() == buildIdx->id()) {
          _indexes.erase(it);
          _indexes.emplace(idx);
          break;
        }
      }
    } else {
      _indexes.emplace(idx);
    }
    guard.unlock();
#if USE_PLAN_CACHE
    arangodb::aql::PlanCache::instance()->invalidate(_logicalCollection.vocbase());
#endif

    // inBackground index might not recover selectivity estimate w/o sync
    if (inBackground && !idx->unique() && idx->hasSelectivityEstimate()) {
      engine->settingsManager()->sync(false);
    }

    // Step 6. persist in rocksdb
    if (!engine->inRecovery()) {  // write new collection marker
      auto builder = _logicalCollection.toVelocyPackIgnore(
          {"path", "statusString"},
          LogicalDataSource::Serialization::PersistenceWithInProgress);
      VPackBuilder indexInfo;
      idx->toVelocyPack(indexInfo, Index::makeFlags(Index::Serialize::Internals));
      res = engine->writeCreateCollectionMarker(_logicalCollection.vocbase().id(),
                                                _logicalCollection.id(), builder.slice(),
                                                RocksDBLogValue::IndexCreate(_logicalCollection.vocbase().id(),
                                                                             _logicalCollection.id(), indexInfo.slice()));
    }
  } while(false);

  // cleanup routine
  if (res.fail()) { // We could not create the index. Better abort
    WRITE_LOCKER(guard, _indexesLock);
    auto it = _indexes.begin();
    while (it != _indexes.end()) {
      if ((*it)->id() == idx->id()) {
        _indexes.erase(it);
        break;
      }
      it++;
    }
    guard.unlock();
    idx->drop();
    THROW_ARANGO_EXCEPTION(res);
  }

  created = true;
  return idx;
}

/// @brief Drop an index with the given iid.
bool RocksDBCollection::dropIndex(TRI_idx_iid_t iid) {
  // usually always called when _exclusiveLock is held
  if (iid == 0) {
    // invalid index id or primary index
    return true;
  }

  std::shared_ptr<arangodb::Index> toRemove;
  {
    WRITE_LOCKER(guard, _indexesLock);
    for (auto& it : _indexes) {
      if (iid == it->id()) {
        toRemove = it;
        _indexes.erase(it);
        break;
      }
    }
  }

  if (!toRemove) {  // index not found
    // We tried to remove an index that does not exist
    events::DropIndex(_logicalCollection.vocbase().name(), _logicalCollection.name(),
                      std::to_string(iid), TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
    return false;
  }

  READ_LOCKER(guard, _indexesLock);

  RocksDBIndex* cindex = static_cast<RocksDBIndex*>(toRemove.get());
  TRI_ASSERT(cindex != nullptr);

  Result res = cindex->drop();

  if (!res.ok()) {
    return false;
  }

  events::DropIndex(_logicalCollection.vocbase().name(), _logicalCollection.name(),
                    std::to_string(iid), TRI_ERROR_NO_ERROR);

  cindex->compact(); // trigger compaction before deleting the object

  auto* engine = static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);

  if (!engine || engine->inRecovery()) {
    return true; // skip writing WAL marker if inRecovery()
  }

  auto builder =  // RocksDB path
      _logicalCollection.toVelocyPackIgnore(
          {"path", "statusString"},
          LogicalDataSource::Serialization::PersistenceWithInProgress);

  // log this event in the WAL and in the collection meta-data
  res = engine->writeCreateCollectionMarker( // write marker
    _logicalCollection.vocbase().id(), // vocbase id
    _logicalCollection.id(), // collection id
    builder.slice(), // RocksDB path
    RocksDBLogValue::IndexDrop( // marker
      _logicalCollection.vocbase().id(), _logicalCollection.id(), iid // args
    )
  );

  return res.ok();
}

std::unique_ptr<IndexIterator> RocksDBCollection::getAllIterator(transaction::Methods* trx) const {
  return std::make_unique<RocksDBAllIndexIterator>(&_logicalCollection, trx);
}

std::unique_ptr<IndexIterator> RocksDBCollection::getAnyIterator(transaction::Methods* trx) const {
  return std::make_unique<RocksDBAnyIndexIterator>(&_logicalCollection, trx);
}

////////////////////////////////////
// -- SECTION DML Operations --
///////////////////////////////////

Result RocksDBCollection::truncate(transaction::Methods& trx, OperationOptions& options) {
  TRI_ASSERT(_objectId != 0);
  auto state = RocksDBTransactionState::toState(&trx);
  RocksDBMethods* mthds = state->rocksdbMethods();
  
  if (state->isOnlyExclusiveTransaction() &&
      state->hasHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE) &&
      this->canUseRangeDeleteInWal() && _meta.numberDocuments() >= 32 * 1024) {
    // non-transactional truncate optimization. We perform a bunch of
    // range deletes and circumvent the normal rocksdb::Transaction.
    // no savepoint needed here
    TRI_ASSERT(!state->hasOperations());  // not allowed
    
    TRI_IF_FAILURE("RocksDBRemoveLargeRangeOn") {
      return Result(TRI_ERROR_DEBUG);
    }
    
    RocksDBEngine* engine = rocksutils::globalRocksEngine();
    rocksdb::DB* db = engine->db()->GetRootDB();
    
    TRI_IF_FAILURE("RocksDBCollection::truncate::forceSync") {
      engine->settingsManager()->sync(false);
    }
    
    // pre commit sequence needed to place a blocker
    rocksdb::SequenceNumber seq = rocksutils::latestSequenceNumber();
    auto guard = scopeGuard([&] {  // remove blocker afterwards
      _meta.removeBlocker(state->id());
    });
    _meta.placeBlocker(state->id(), seq);
    
    rocksdb::WriteBatch batch;
    // delete documents
    RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(_objectId);
    rocksdb::Status s =
    batch.DeleteRange(bounds.columnFamily(), bounds.start(), bounds.end());
    if (!s.ok()) {
      return rocksutils::convertStatus(s);
    }
    
    // delete indexes, place estimator blockers
    {
      READ_LOCKER(idxGuard, _indexesLock);
      for (std::shared_ptr<Index> const& idx : _indexes) {
        RocksDBIndex* ridx = static_cast<RocksDBIndex*>(idx.get());
        bounds = ridx->getBounds();
        s = batch.DeleteRange(bounds.columnFamily(), bounds.start(), bounds.end());
        if (!s.ok()) {
          return rocksutils::convertStatus(s);
        }
      }
    }
    
    // add the log entry so we can recover the correct count
    auto log = RocksDBLogValue::CollectionTruncate(trx.vocbase().id(),
                                                   _logicalCollection.id(), _objectId);
    
    s = batch.PutLogData(log.slice());
    
    if (!s.ok()) {
      return rocksutils::convertStatus(s);
    }
    
    rocksdb::WriteOptions wo;
    
    s = db->Write(wo, &batch);
    
    if (!s.ok()) {
      return rocksutils::convertStatus(s);
    }
    
    seq = db->GetLatestSequenceNumber() - 1;  // post commit sequence
    
    uint64_t numDocs = _meta.numberDocuments();
    _meta.adjustNumberDocuments(seq, /*revision*/ newRevisionId(),
                                -static_cast<int64_t>(numDocs));
    
    {
      READ_LOCKER(guard, _indexesLock);
      for (std::shared_ptr<Index> const& idx : _indexes) {
        idx->afterTruncate(seq);  // clears caches / clears links (if applicable)
      }
    }
    
    guard.fire();  // remove blocker
    
    TRI_ASSERT(!state->hasOperations());  // not allowed
    return Result{};
  }
  
  TRI_IF_FAILURE("RocksDBRemoveLargeRangeOff") {
    return Result(TRI_ERROR_DEBUG);
  }
  
  // normal transactional truncate
  RocksDBKeyBounds documentBounds = RocksDBKeyBounds::CollectionDocuments(_objectId);
  rocksdb::Comparator const* cmp = RocksDBColumnFamily::documents()->GetComparator();
  // intentionally copy the read options so we can modify them
  rocksdb::ReadOptions ro = mthds->iteratorReadOptions();
  rocksdb::Slice const end = documentBounds.end();
  ro.iterate_upper_bound = &end;
  // we are going to blow away all data anyway. no need to blow up the cache
  ro.fill_cache = false;
  
  TRI_ASSERT(ro.snapshot);
  
  // avoid OOM error for truncate by committing earlier
  uint64_t const prvICC = state->options().intermediateCommitCount;
  state->options().intermediateCommitCount = std::min<uint64_t>(prvICC, 10000);
  
  uint64_t found = 0;
  VPackBuilder docBuffer;
  auto iter = mthds->NewIterator(ro, documentBounds.columnFamily());
  for (iter->Seek(documentBounds.start());
       iter->Valid() && cmp->Compare(iter->key(), end) < 0;
       iter->Next()) {
    
    ++found;
    TRI_ASSERT(_objectId == RocksDBKey::objectId(iter->key()));
    VPackSlice document(reinterpret_cast<uint8_t const*>(iter->value().data()));
    TRI_ASSERT(document.isObject());
    
    // tmp may contain a pointer into rocksdb::WriteBuffer::_rep. This is
    // a 'std::string' which might be realloc'ed on any Put/Delete operation
    docBuffer.clear();
    docBuffer.add(document);
    
    // To print the WAL we need key and RID
    VPackSlice key;
    TRI_voc_rid_t rid = 0;
    transaction::helpers::extractKeyAndRevFromDocument(document, key, rid);
    TRI_ASSERT(key.isString());
    TRI_ASSERT(rid != 0);

    RocksDBSavePoint guard(&trx, TRI_VOC_DOCUMENT_OPERATION_REMOVE);
    state->prepareOperation(_logicalCollection.id(),
                            rid,  // actual revision ID!!
                            TRI_VOC_DOCUMENT_OPERATION_REMOVE);

    LocalDocumentId const docId = RocksDBKey::documentId(iter->key());
    auto res = removeDocument(&trx, docId, docBuffer.slice(), options);

    if (res.fail()) {  // Failed to remove document in truncate.
      return res;
    }

    bool hasPerformedIntermediateCommit = false;
    res = state->addOperation(_logicalCollection.id(), docId.id(), TRI_VOC_DOCUMENT_OPERATION_REMOVE,
                              hasPerformedIntermediateCommit);

    if (res.fail()) {  // This should never happen...
      return res;
    }
    guard.finish(hasPerformedIntermediateCommit);

    trackWaitForSync(&trx, options);

  }

  // reset to previous value after truncate is finished
  state->options().intermediateCommitCount = prvICC;

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (state->numCommits() == 0) {
    // check IN TRANSACTION if documents have been deleted
    if (mthds->countInBounds(RocksDBKeyBounds::CollectionDocuments(_objectId), true)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "deletion check in collection truncate "
                                     "failed - not all documents have been "
                                     "deleted");
    }
  }
#endif

  TRI_IF_FAILURE("FailAfterAllCommits") { return Result(TRI_ERROR_DEBUG); }
  TRI_IF_FAILURE("SegfaultAfterAllCommits") {
    TRI_TerminateDebugging("SegfaultAfterAllCommits");
  }
  return Result{};
}

LocalDocumentId RocksDBCollection::lookupKey(transaction::Methods* trx,
                                             VPackSlice const& key) const {
  TRI_ASSERT(key.isString());
  return primaryIndex()->lookupKey(trx, arangodb::velocypack::StringRef(key));
}

bool RocksDBCollection::lookupRevision(transaction::Methods* trx, VPackSlice const& key,
                                       TRI_voc_rid_t& revisionId) const {
  TRI_ASSERT(key.isString());
  LocalDocumentId documentId;
  revisionId = 0;
  // lookup the revision id in the primary index
  if (!primaryIndex()->lookupRevision(trx, arangodb::velocypack::StringRef(key),
                                      documentId, revisionId)) {
    // document not found
    TRI_ASSERT(revisionId == 0);
    return false;
  }

  // document found, but revisionId may not have been present in the primary
  // index. this can happen for "older" collections
  TRI_ASSERT(documentId.isSet());

  // now look up the revision id in the actual document data

  return readDocumentWithCallback(trx, documentId, [&revisionId](LocalDocumentId const&, VPackSlice doc) {
    revisionId = transaction::helpers::extractRevFromDocument(doc);
    return true;
  });
}

Result RocksDBCollection::read(transaction::Methods* trx,
                               arangodb::velocypack::StringRef const& key,
                               ManagedDocumentResult& result, bool /*lock*/) {
  Result res;
  do {
    LocalDocumentId const documentId = primaryIndex()->lookupKey(trx, key);
    if (!documentId.isSet()) {
      res.reset(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
      break;
    }  // else found
    
    std::string* buffer = result.setManaged();
    rocksdb::PinnableSlice ps(buffer);
    res = lookupDocumentVPack(trx, documentId, ps, /*readCache*/true, /*fillCache*/true);
    if (res.ok()) {
      if (ps.IsPinned()) {
        buffer->assign(ps.data(), ps.size());
      } // else value is already assigned
      result.setRevisionId(); // extracts id from buffer
    }
  } while(res.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) &&
          RocksDBTransactionState::toState(trx)->setSnapshotOnReadOnly());
  return res;
}

// read using a token!
bool RocksDBCollection::readDocument(transaction::Methods* trx,
                                     LocalDocumentId const& documentId,
                                     ManagedDocumentResult& result) const {
  if (documentId.isSet()) {
    std::string* buffer = result.setManaged();
    rocksdb::PinnableSlice ps(buffer);
    Result res = lookupDocumentVPack(trx, documentId, ps, /*readCache*/true, /*fillCache*/true);
    if (res.ok()) {
      if (ps.IsPinned()) {
        buffer->assign(ps.data(), ps.size());
      } // else value is already assigned
      return true;
    }
  }
  return false;
}

// read using a token!
bool RocksDBCollection::readDocumentWithCallback(transaction::Methods* trx,
                                                 LocalDocumentId const& documentId,
                                                 IndexIterator::DocumentCallback const& cb) const {
  if (documentId.isSet()) {
    return lookupDocumentVPack(trx, documentId, cb, /*withCache*/true);
  }
  return false;
}

Result RocksDBCollection::insert(arangodb::transaction::Methods* trx,
                                 arangodb::velocypack::Slice const slice,
                                 arangodb::ManagedDocumentResult& resultMdr,
                                 OperationOptions& options,
                                 bool /*lock*/, KeyLockInfo* /*keyLockInfo*/,
                                 std::function<void()> const& cbDuringLock) {

  bool const isEdgeCollection = (TRI_COL_TYPE_EDGE == _logicalCollection.type());

  transaction::BuilderLeaser builder(trx);
  TRI_voc_tick_t revisionId;
  Result res(newObjectForInsert(trx, slice, isEdgeCollection, *builder.get(),
                                options.isRestore, revisionId));
  if (res.fail()) {
    return res;
  }

  VPackSlice newSlice = builder->slice();
  if (options.overwrite) {
    // special optimization for the overwrite case:
    // in case the operation is a RepSert, we will first check if the specified
    // primary key exists. we can abort this low-level insert early, before any
    // modification to the data has been done. this saves us from creating a
    // RocksDB transaction SavePoint. if we don't do the check here, we will
    // always create a SavePoint first and insert the new document. when then
    // inserting the key for the primary index and then detecting a unique
    // constraint violation, the transaction would be rolled back to the
    // SavePoint state, which will rebuild *all* data in the WriteBatch up to
    // the SavePoint. this can be super-expensive for bigger transactions. to
    // keep things simple, we are not checking for unique constraint violations
    // in secondary indexes here, but defer it to the regular index insertion
    // check
    VPackSlice keySlice = transaction::helpers::extractKeyFromDocument(newSlice);
    if (keySlice.isString()) {
      LocalDocumentId const oldDocumentId =
          primaryIndex()->lookupKey(trx, arangodb::velocypack::StringRef(keySlice));
      if (oldDocumentId.isSet()) {
        if (options.indexOperationMode == Index::OperationMode::internal) {
          // need to return the key of the conflict document
          return res.reset(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED,
                           keySlice.copyString());
        }
        return res.reset(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED);
      }
    }
  }

  LocalDocumentId const documentId = LocalDocumentId::create();

  RocksDBSavePoint guard(trx, TRI_VOC_DOCUMENT_OPERATION_INSERT);

  auto* state = RocksDBTransactionState::toState(trx);
  state->prepareOperation(_logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_INSERT);

  res = insertDocument(trx, documentId, newSlice, options);

  if (res.ok()) {
    trackWaitForSync(trx, options);

    if (options.returnNew) {
      resultMdr.setManaged(newSlice.begin());
      TRI_ASSERT(resultMdr.revisionId() == revisionId);
    } else if (!options.silent) {  //  need to pass revId manually
      transaction::BuilderLeaser keyBuilder(trx);
      keyBuilder->openObject(/*unindexed*/true);
      keyBuilder->add(StaticStrings::KeyString, transaction::helpers::extractKeyFromDocument(newSlice));
      keyBuilder->close();
      resultMdr.setManaged()->assign(reinterpret_cast<char const*>(keyBuilder->start()),
                                     keyBuilder->size());
      resultMdr.setRevisionId(revisionId);
    }

    bool hasPerformedIntermediateCommit = false;
    res = state->addOperation(_logicalCollection.id(), revisionId,
                              TRI_VOC_DOCUMENT_OPERATION_INSERT,
                              hasPerformedIntermediateCommit);

    if (res.ok() && cbDuringLock != nullptr) {
      cbDuringLock();
    }

    guard.finish(hasPerformedIntermediateCommit);
  }

  return res;
}

Result RocksDBCollection::update(arangodb::transaction::Methods* trx,
                                 arangodb::velocypack::Slice const newSlice,
                                 ManagedDocumentResult& resultMdr, OperationOptions& options,
                                 bool /*lock*/, ManagedDocumentResult& previousMdr) {

  VPackSlice keySlice = newSlice.get(StaticStrings::KeyString);
  if (keySlice.isNone()) {
    return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
  } else if (!keySlice.isString()) {
    return TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD;
  }

  auto const oldDocumentId = primaryIndex()->lookupKey(trx, VPackStringRef(keySlice));
  if (!oldDocumentId.isSet()) {
    return TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND;
  }
  std::string* prevBuffer = previousMdr.setManaged();
  // uses either prevBuffer or avoids memcpy (if read hits block cache)
  rocksdb::PinnableSlice previousPS(prevBuffer);
  Result res = lookupDocumentVPack(trx, oldDocumentId, previousPS,
                                   /*readCache*/true, /*fillCache*/false);
  if (res.fail()) {
    return res;
  }

  TRI_ASSERT(previousPS.size() > 0);
  VPackSlice const oldDoc(reinterpret_cast<uint8_t const*>(previousPS.data()));
  previousMdr.setRevisionId(transaction::helpers::extractRevFromDocument(oldDoc));
  TRI_ASSERT(previousMdr.revisionId() != 0);

  if (!options.ignoreRevs) {  // Check old revision:
    TRI_voc_rid_t expectedRev = TRI_ExtractRevisionId(newSlice);
    int result = checkRevision(trx, expectedRev, previousMdr.revisionId());
    if (result != TRI_ERROR_NO_ERROR) {
      return res.reset(result);
    }
  }

  if (newSlice.length() <= 1) {  // TODO move above ?!
    // shortcut. no need to do anything
    resultMdr.setManaged(oldDoc.begin());
    TRI_ASSERT(!resultMdr.empty());

    trackWaitForSync(trx, options);
    return res;
  }

  // merge old and new values
  TRI_voc_rid_t revisionId;
  LocalDocumentId const newDocumentId = LocalDocumentId::create();
  auto isEdgeCollection = (TRI_COL_TYPE_EDGE == _logicalCollection.type());

  transaction::BuilderLeaser builder(trx);
  res = mergeObjectsForUpdate(trx, oldDoc, newSlice, isEdgeCollection,
                              options.mergeObjects, options.keepNull,
                              *builder.get(), options.isRestore, revisionId);
  if (res.fail()) {
    return res;
  }

  if (_isDBServer) {
    // Need to check that no sharding keys have changed:
    if (arangodb::shardKeysChanged(_logicalCollection, oldDoc, builder->slice(), true)) {
      return res.reset(TRI_ERROR_CLUSTER_MUST_NOT_CHANGE_SHARDING_ATTRIBUTES);
    }
    if (arangodb::smartJoinAttributeChanged(_logicalCollection, oldDoc, builder->slice(), true)) {
      return res.reset(TRI_ERROR_CLUSTER_MUST_NOT_CHANGE_SMART_JOIN_ATTRIBUTE);
    }
  }

  VPackSlice const newDoc(builder->slice());
  RocksDBSavePoint guard(trx, TRI_VOC_DOCUMENT_OPERATION_UPDATE);

  auto* state = RocksDBTransactionState::toState(trx);
  // add possible log statement under guard
  state->prepareOperation(_logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_UPDATE);
  res = updateDocument(trx, oldDocumentId, oldDoc, newDocumentId, newDoc, options);

  if (res.ok()) {
    trackWaitForSync(trx, options);

    if (options.returnNew) {
      resultMdr.setManaged(newDoc.begin());
      TRI_ASSERT(!resultMdr.empty());
    } else {  //  need to pass revId manually
      resultMdr.setRevisionId(revisionId);
    }
    if (options.returnOld) {
      if (previousPS.IsPinned()) { // value was not copied
        prevBuffer->assign(previousPS.data(), previousPS.size());
      }  // else value is already assigned
      TRI_ASSERT(!previousMdr.empty());
    } else {
      previousMdr.clearData();
    }

    bool hasPerformedIntermediateCommit = false;
    auto result = state->addOperation(_logicalCollection.id(), revisionId,
                                      TRI_VOC_DOCUMENT_OPERATION_UPDATE,
                                      hasPerformedIntermediateCommit);
    if (result.fail()) {
      THROW_ARANGO_EXCEPTION(result);
    }

    guard.finish(hasPerformedIntermediateCommit);
  }

  return res;
}

Result RocksDBCollection::replace(transaction::Methods* trx,
                                  arangodb::velocypack::Slice const newSlice,
                                  ManagedDocumentResult& resultMdr, OperationOptions& options,
                                  bool /*lock*/, ManagedDocumentResult& previousMdr) {

  VPackSlice keySlice = newSlice.get(StaticStrings::KeyString);
  if (keySlice.isNone()) {
    return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
  } else if (!keySlice.isString()) {
    return TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD;
  }

  auto const oldDocumentId = primaryIndex()->lookupKey(trx, VPackStringRef(keySlice));
  if (!oldDocumentId.isSet()) {
    return TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND;
  }
  std::string* prevBuffer = previousMdr.setManaged();
  // uses either prevBuffer or avoids memcpy (if read hits block cache)
  rocksdb::PinnableSlice previousPS(prevBuffer);
  Result res = lookupDocumentVPack(trx, oldDocumentId, previousPS,
                                   /*readCache*/true, /*fillCache*/false);
  if (res.fail()) {
    return res;
  }

  TRI_ASSERT(previousPS.size() > 0);
  VPackSlice const oldDoc(reinterpret_cast<uint8_t const*>(previousPS.data()));
  previousMdr.setRevisionId(transaction::helpers::extractRevFromDocument(oldDoc));
  TRI_ASSERT(previousMdr.revisionId() != 0);

  if (!options.ignoreRevs) {  // Check old revision:
    TRI_voc_rid_t expectedRev = TRI_ExtractRevisionId(newSlice);
    res = checkRevision(trx, expectedRev, previousMdr.revisionId());
    if (res.fail()) {
      return res;
    }
  }

  // merge old and new values
  TRI_voc_rid_t revisionId;
  LocalDocumentId const newDocumentId = LocalDocumentId::create();
  bool const isEdgeCollection = (TRI_COL_TYPE_EDGE == _logicalCollection.type());

  transaction::BuilderLeaser builder(trx);
  res = newObjectForReplace(trx, oldDoc, newSlice, isEdgeCollection,
                            *builder.get(), options.isRestore, revisionId);
  if (res.fail()) {
    return res;
  }

  if (_isDBServer) {
    // Need to check that no sharding keys have changed:
    if (arangodb::shardKeysChanged(_logicalCollection, oldDoc, builder->slice(), false)) {
      return res.reset(TRI_ERROR_CLUSTER_MUST_NOT_CHANGE_SHARDING_ATTRIBUTES);
    }
    if (arangodb::smartJoinAttributeChanged(_logicalCollection, oldDoc, builder->slice(), false)) {
      return Result(TRI_ERROR_CLUSTER_MUST_NOT_CHANGE_SMART_JOIN_ATTRIBUTE);
    }
  }

  VPackSlice const newDoc(builder->slice());
  RocksDBSavePoint guard(trx, TRI_VOC_DOCUMENT_OPERATION_REPLACE);

  auto* state = RocksDBTransactionState::toState(trx);
  // add possible log statement under guard
  state->prepareOperation(_logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_REPLACE);
  res = updateDocument(trx, oldDocumentId, oldDoc, newDocumentId, newDoc, options);

  if (res.ok()) {
    trackWaitForSync(trx, options);

    if (options.returnNew) {
      resultMdr.setManaged(newDoc.begin());
      TRI_ASSERT(!resultMdr.empty());
    } else {  //  need to pass revId manually
      resultMdr.setRevisionId(revisionId);
    }
    if (options.returnOld) {
      if (previousPS.IsPinned()) { // value was not copied
        prevBuffer->assign(previousPS.data(), previousPS.size());
      }  // else value is already assigned
      TRI_ASSERT(!previousMdr.empty());
    } else {
      previousMdr.clearData();
    }

    bool hasPerformedIntermediateCommit = false;
    auto result = state->addOperation(_logicalCollection.id(), revisionId,
                                      TRI_VOC_DOCUMENT_OPERATION_REPLACE,
                                      hasPerformedIntermediateCommit);

    if (result.fail()) {
      THROW_ARANGO_EXCEPTION(result);
    }

    guard.finish(hasPerformedIntermediateCommit);
  }

  return res;
}

Result RocksDBCollection::remove(transaction::Methods& trx, velocypack::Slice slice,
                                 ManagedDocumentResult& previousMdr, OperationOptions& options,
                                 bool /*lock*/, KeyLockInfo* /*keyLockInfo*/,
                                 std::function<void()> const& cbDuringLock) {

  VPackSlice keySlice;
  if (slice.isString()) {
    keySlice = slice;
  } else {
    keySlice = slice.get(StaticStrings::KeyString);
  }
  TRI_ASSERT(!keySlice.isNone());
  if (!keySlice.isString()) {
    return TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD;
  }

  auto const documentId = primaryIndex()->lookupKey(&trx, VPackStringRef(keySlice));
  if (!documentId.isSet()) {
    return TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND;
  }
  std::string* prevBuffer = previousMdr.setManaged();
  // uses either prevBuffer or avoids memcpy (if read hits block cache)
  rocksdb::PinnableSlice previousPS(prevBuffer);
  Result res = lookupDocumentVPack(&trx, documentId, previousPS,
                                   /*readCache*/true, /*fillCache*/false);
  if (res.fail()) {
    return res;
  }

  TRI_ASSERT(previousPS.size() > 0);
  VPackSlice const oldDoc(reinterpret_cast<uint8_t const*>(previousPS.data()));
  previousMdr.setRevisionId(transaction::helpers::extractRevFromDocument(oldDoc));
  TRI_ASSERT(previousMdr.revisionId() != 0);

  // Check old revision:
  if (!options.ignoreRevs && slice.isObject()) {
    TRI_voc_rid_t expectedRevisionId = TRI_ExtractRevisionId(slice);
    res = checkRevision(&trx, expectedRevisionId, previousMdr.revisionId());

    if (res.fail()) {
      return res;
    }
  }

  auto state = RocksDBTransactionState::toState(&trx);
  RocksDBSavePoint guard(&trx, TRI_VOC_DOCUMENT_OPERATION_REMOVE);

  // add possible log statement under guard
  state->prepareOperation(_logicalCollection.id(), previousMdr.revisionId(),
                          TRI_VOC_DOCUMENT_OPERATION_REMOVE);
  res = removeDocument(&trx, documentId, oldDoc, options);

  if (res.ok()) {
    trackWaitForSync(&trx, options);

    if (options.returnOld) {
      if (previousPS.IsPinned()) {  // value was not copied
        prevBuffer->assign(previousPS.data(), previousPS.size());
      }  // else value is already assigned
      TRI_ASSERT(!previousMdr.empty());
    } else {
      previousMdr.clearData();
    }

    bool hasPerformedIntermediateCommit = false;
    res = state->addOperation(_logicalCollection.id(), newRevisionId(), TRI_VOC_DOCUMENT_OPERATION_REMOVE,
                              hasPerformedIntermediateCommit);

    if (res.ok() && cbDuringLock != nullptr) {
      cbDuringLock();
    }

    guard.finish(hasPerformedIntermediateCommit);
  }

  return res;
}

/// @brief return engine-specific figures
void RocksDBCollection::figuresSpecific(arangodb::velocypack::Builder& builder) {
  rocksdb::TransactionDB* db = rocksutils::globalRocksDB();
  RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(_objectId);
  rocksdb::Range r(bounds.start(), bounds.end());

  uint64_t out = 0;
  db->GetApproximateSizes(RocksDBColumnFamily::documents(), &r, 1, &out,
                          static_cast<uint8_t>(
                              rocksdb::DB::SizeApproximationFlags::INCLUDE_MEMTABLES |
                              rocksdb::DB::SizeApproximationFlags::INCLUDE_FILES));

  builder.add("documentsSize", VPackValue(out));
  bool cacheInUse = useCache();
  builder.add("cacheInUse", VPackValue(cacheInUse));
  if (cacheInUse) {
    builder.add("cacheSize", VPackValue(_cache->size()));
    builder.add("cacheUsage", VPackValue(_cache->usage()));
    auto hitRates = _cache->hitRates();
    double rate = hitRates.first;
    rate = std::isnan(rate) ? 0.0 : rate;
    builder.add("cacheLifeTimeHitRate", VPackValue(rate));
    rate = hitRates.second;
    rate = std::isnan(rate) ? 0.0 : rate;
    builder.add("cacheWindowedHitRate", VPackValue(rate));
  } else {
    builder.add("cacheSize", VPackValue(0));
    builder.add("cacheUsage", VPackValue(0));
  }
}

namespace {
template<typename F>
void reverseIdxOps(PhysicalCollection::IndexContainerType const& indexes,
                   PhysicalCollection::IndexContainerType::const_iterator& it,
                   F&& op) {
  while (it != indexes.begin()) {
    it--;
    auto* rIdx = static_cast<RocksDBIndex*>(it->get());
    if (rIdx->needsReversal()) {
      if (std::forward<F>(op)(rIdx).fail()) {
        // best effort for reverse failed. Let`s trigger full rollback  
        // or we will end up with inconsistent storage and indexes
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "Failed to reverse index operation.");
      }
    }
  }
}
}

Result RocksDBCollection::insertDocument(arangodb::transaction::Methods* trx,
                                         LocalDocumentId const& documentId,
                                         VPackSlice const& doc,
                                         OperationOptions& options) const {
  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  Result res;

  RocksDBKeyLeaser key(trx);
  key->constructDocument(_objectId, documentId);

  RocksDBTransactionState* state = RocksDBTransactionState::toState(trx);
  if (state->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED)) {
    // blacklist new document to avoid caching without committing first
    blackListKey(key.ref());
  }
    
  RocksDBMethods* mthds = state->rocksdbMethods();
  // disable indexing in this transaction if we are allowed to
  IndexingDisabler disabler(mthds, state->isSingleOperation());

  TRI_ASSERT(key->containsLocalDocumentId(documentId));
  rocksdb::Status s =
      mthds->PutUntracked(RocksDBColumnFamily::documents(), key.ref(),
                          rocksdb::Slice(doc.startAs<char>(),
                                         static_cast<size_t>(doc.byteSize())));
  if (!s.ok()) {
    return res.reset(rocksutils::convertStatus(s, rocksutils::document));
  }

  READ_LOCKER(guard, _indexesLock);
  
  bool needReversal = false;
  for (auto it = _indexes.begin(); it != _indexes.end(); it++) {
    RocksDBIndex* rIdx = static_cast<RocksDBIndex*>(it->get());
    res = rIdx->insert(*trx, mthds, documentId, doc, options.indexOperationMode);
    needReversal = needReversal || rIdx->needsReversal();
    if (res.fail()) {
      if (needReversal && !state->isSingleOperation()) {
        ::reverseIdxOps(_indexes, it, [mthds, trx, &documentId, &doc](RocksDBIndex* rid) {
          return rid->remove(*trx, mthds, documentId, doc,  Index::OperationMode::rollback);
        });
      }
      break;
    }
  }

  return res;
}

Result RocksDBCollection::removeDocument(arangodb::transaction::Methods* trx,
                                         LocalDocumentId const& documentId,
                                         VPackSlice const& doc,
                                         OperationOptions& options) const {
  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(_objectId != 0);
  Result res;

  RocksDBKeyLeaser key(trx);
  key->constructDocument(_objectId, documentId);

  blackListKey(key.ref());

  RocksDBMethods* mthds = RocksDBTransactionState::toMethods(trx);

  // disable indexing in this transaction if we are allowed to
  IndexingDisabler disabler(mthds, trx->isSingleOperationTransaction());

  rocksdb::Status s = mthds->SingleDelete(RocksDBColumnFamily::documents(), key.ref());
  if (!s.ok()) {
    return res.reset(rocksutils::convertStatus(s, rocksutils::document));
  }

  /*LOG_TOPIC("17502", ERR, Logger::ENGINES)
      << "Delete rev: " << revisionId << " trx: " << trx->state()->id()
      << " seq: " << mthds->sequenceNumber()
      << " objectID " << _objectId << " name: " << _logicalCollection.name();*/

  READ_LOCKER(guard, _indexesLock);
  bool needReversal = false;
  for (auto it = _indexes.begin(); it != _indexes.end(); it++) {
    RocksDBIndex* rIdx = static_cast<RocksDBIndex*>(it->get());
    res = rIdx->remove(*trx, mthds, documentId, doc, options.indexOperationMode);
    needReversal = needReversal || rIdx->needsReversal();
    if (res.fail()) {
      if (needReversal && !trx->isSingleOperationTransaction()) {
        ::reverseIdxOps(_indexes, it, [mthds, trx, &documentId, &doc](RocksDBIndex* rid) {
          return rid->insert(*trx, mthds, documentId, doc, Index::OperationMode::rollback);
        });
      }
      break;
    }
  }

  return res;
}

Result RocksDBCollection::updateDocument(transaction::Methods* trx,
                                         LocalDocumentId const& oldDocumentId,
                                         VPackSlice const& oldDoc,
                                         LocalDocumentId const& newDocumentId,
                                         VPackSlice const& newDoc,
                                         OperationOptions& options) const {
  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(_objectId != 0);
  Result res;

  RocksDBTransactionState* state = RocksDBTransactionState::toState(trx);
  RocksDBMethods* mthds = state->rocksdbMethods();
  // disable indexing in this transaction if we are allowed to
  IndexingDisabler disabler(mthds, trx->isSingleOperationTransaction());

  RocksDBKeyLeaser key(trx);
  key->constructDocument(_objectId, oldDocumentId);
  TRI_ASSERT(key->containsLocalDocumentId(oldDocumentId));
  blackListKey(key.ref());

  rocksdb::Status s = mthds->SingleDelete(RocksDBColumnFamily::documents(), key.ref());
  if (!s.ok()) {
    return res.reset(rocksutils::convertStatus(s, rocksutils::document));
  }

  key->constructDocument(_objectId, newDocumentId);
  TRI_ASSERT(key->containsLocalDocumentId(newDocumentId));
  s = mthds->PutUntracked(RocksDBColumnFamily::documents(), key.ref(),
                          rocksdb::Slice(newDoc.startAs<char>(),
                                         static_cast<size_t>(newDoc.byteSize())));
  if (!s.ok()) {
    return res.reset(rocksutils::convertStatus(s, rocksutils::document));
  }
  
  if (state->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED)) {
    // blacklist new document to avoid caching without committing first
    blackListKey(key.ref());
  }
    
  READ_LOCKER(guard, _indexesLock);
  bool needReversal = false;
  for (auto it = _indexes.begin(); it != _indexes.end(); it++) {
    auto rIdx = static_cast<RocksDBIndex*>(it->get());
    res = rIdx->update(*trx, mthds, oldDocumentId, oldDoc, newDocumentId,
                       newDoc, options.indexOperationMode);
    needReversal = needReversal || rIdx->needsReversal();
    if (!res.ok()) {
      if (needReversal && !trx->isSingleOperationTransaction()) {
        ::reverseIdxOps(_indexes, it,
                        [mthds, trx, &newDocumentId, &newDoc, &oldDocumentId, &oldDoc](RocksDBIndex* rid) {
                          return rid->update(*trx, mthds, newDocumentId, newDoc, oldDocumentId,
                                             oldDoc, Index::OperationMode::rollback);
                        });
      }
      break;
    }
  }
  return res;
}

/// @brief lookup document in cache and / or rocksdb
/// @param readCache attempt to read from cache
/// @param fillCache fill cache with found document
arangodb::Result RocksDBCollection::lookupDocumentVPack(transaction::Methods* trx,
                                                        LocalDocumentId const& documentId,
                                                        rocksdb::PinnableSlice& ps,
                                                        bool readCache,
                                                        bool fillCache) const {
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(_objectId != 0);
  Result res;

  RocksDBKeyLeaser key(trx);
  key->constructDocument(_objectId, documentId);

  bool lockTimeout = false;
  if (readCache && useCache()) {
    TRI_ASSERT(_cache != nullptr);
    // check cache first for fast path
    auto f = _cache->find(key->string().data(),
                          static_cast<uint32_t>(key->string().size()));
    if (f.found()) {  // copy finding into buffer
      ps.PinSelf(rocksdb::Slice(reinterpret_cast<char const*>(f.value()->value()),
                                f.value()->valueSize()));
      // TODO we could potentially use the PinSlice method ?!
      return res; // all good
    }
    if (f.result().errorNumber() == TRI_ERROR_LOCK_TIMEOUT) {
      // assuming someone is currently holding a write lock, which
      // is why we cannot access the TransactionalBucket.
      lockTimeout = true;  // we skip the insert in this case
    }
  }

  RocksDBMethods* mthd = RocksDBTransactionState::toMethods(trx);
  rocksdb::Status s = mthd->Get(RocksDBColumnFamily::documents(), key->string(), &ps);

  if (!s.ok()) {
    LOG_TOPIC("f63dd", DEBUG, Logger::ENGINES)
    << "NOT FOUND rev: " << documentId.id() << " trx: " << trx->state()->id()
    << " objectID " << _objectId << " name: " << _logicalCollection.name();
    return res.reset(rocksutils::convertStatus(s, rocksutils::document));
  }

  if (fillCache && useCache() && !lockTimeout) {
    TRI_ASSERT(_cache != nullptr);
    // write entry back to cache
    auto entry =
        cache::CachedValue::construct(key->string().data(),
                                      static_cast<uint32_t>(key->string().size()),
                                      ps.data(), static_cast<uint64_t>(ps.size()));
    if (entry) {
      auto status = _cache->insert(entry);
      if (status.errorNumber() == TRI_ERROR_LOCK_TIMEOUT) {
        // the writeLock uses cpu_relax internally, so we can try yield
        std::this_thread::yield();
        status = _cache->insert(entry);
      }
      if (status.fail()) {
        delete entry;
      }
    }
  }

  return res;
}

bool RocksDBCollection::lookupDocumentVPack(transaction::Methods* trx,
                                            LocalDocumentId const& documentId,
                                            IndexIterator::DocumentCallback const& cb,
                                            bool withCache) const {

  if (withCache && useCache()) {
    RocksDBKeyLeaser key(trx);
    key->constructDocument(_objectId, documentId);
    TRI_ASSERT(_cache != nullptr);
    // check cache first for fast path
    auto f = _cache->find(key->string().data(),
                          static_cast<uint32_t>(key->string().size()));
    if (f.found()) {
      cb(documentId, VPackSlice(reinterpret_cast<uint8_t const*>(f.value()->value())));
      return true;
    }
  }

  transaction::StringLeaser buffer(trx);
  rocksdb::PinnableSlice ps(buffer.get());
  Result res = lookupDocumentVPack(trx, documentId, ps, /*readCache*/false, withCache);
  if (res.ok()) {
    TRI_ASSERT(ps.size() > 0);
    cb(documentId, VPackSlice(reinterpret_cast<uint8_t const*>(ps.data())));
    return true;
  }
  return false;
}

void RocksDBCollection::createCache() const {
  if (!_cacheEnabled || _cache || _logicalCollection.isAStub() ||
      ServerState::instance()->isCoordinator()) {
    // we leave this if we do not need the cache
    // or if cache already created
    return;
  }

  TRI_ASSERT(_cacheEnabled);
  TRI_ASSERT(_cache.get() == nullptr);
  TRI_ASSERT(CacheManagerFeature::MANAGER != nullptr);
  LOG_TOPIC("f5df2", DEBUG, Logger::CACHE) << "Creating document cache";
  _cache = CacheManagerFeature::MANAGER->createCache(cache::CacheType::Transactional);
  TRI_ASSERT(_cacheEnabled);
}

void RocksDBCollection::destroyCache() const {
  if (!_cache) {
    return;
  }
  TRI_ASSERT(CacheManagerFeature::MANAGER != nullptr);
  // must have a cache...
  TRI_ASSERT(_cache.get() != nullptr);
  LOG_TOPIC("7137b", DEBUG, Logger::CACHE) << "Destroying document cache";
  CacheManagerFeature::MANAGER->destroyCache(_cache);
  _cache.reset();
}

// blacklist given key from transactional cache
void RocksDBCollection::blackListKey(RocksDBKey const& k) const {
  if (useCache()) {
    TRI_ASSERT(_cache != nullptr);
    bool blacklisted = false;
    while (!blacklisted) {
      auto status = _cache->blacklist(k.buffer()->data(),
                                      static_cast<uint32_t>(k.buffer()->size()));
      if (status.ok()) {
        blacklisted = true;
      } else if (status.errorNumber() == TRI_ERROR_SHUTTING_DOWN) {
        destroyCache();
        break;
      }
    }
  }
}

/// @brief can use non transactional range delete in write ahead log
bool RocksDBCollection::canUseRangeDeleteInWal() const {
  if (ServerState::instance()->isSingleServer()) {
    // disableWalFilePruning is used by createIndex
    return _numIndexCreations.load(std::memory_order_acquire) == 0;
  }
  return false;
}
