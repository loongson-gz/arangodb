////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "RestCollectionHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Futures/Utilities.h"
#include "Logger/LogMacros.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/Events.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

RestCollectionHandler::RestCollectionHandler(application_features::ApplicationServer& server,
                                             GeneralRequest* request,
                                             GeneralResponse* response)
    : RestVocbaseBaseHandler(server, request, response) {}

RestStatus RestCollectionHandler::execute() {
  switch (_request->requestType()) {
    case rest::RequestType::GET:
      return handleCommandGet();
    case rest::RequestType::POST:
      handleCommandPost();
      break;
    case rest::RequestType::PUT:
      return handleCommandPut();
    case rest::RequestType::DELETE_REQ:
      handleCommandDelete();
      break;
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
  }

  return RestStatus::DONE;
}

void RestCollectionHandler::shutdownExecute(bool isFinalized) noexcept {
  if (isFinalized) {
    // reset the transactions so they release all locks as early as possible
    _activeTrx.reset();
    _ctxt.reset();
  }
}

RestStatus RestCollectionHandler::handleCommandGet() {
  RestStatus status = RestStatus::DONE;
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  // /_api/collection
  if (suffixes.empty()) {
    bool excludeSystem = _request->parsedValue("excludeSystem", false);

    _builder.openArray();
    methods::Collections::enumerate(&_vocbase, [&](std::shared_ptr<LogicalCollection> const& coll) -> void {
      TRI_ASSERT(coll);
      bool canUse = ExecContext::current().canUseCollection(coll->name(), auth::Level::RO);

      if (canUse && (!excludeSystem || !coll->system())) {
        // We do not need a transaction here
        methods::Collections::Context ctxt(_vocbase, *coll);

        collectionRepresentation(ctxt,
                                 /*showProperties*/ false,
                                 /*showFigures*/ false, /*showCount*/ false,
                                 /*detailedCount*/ false);
      }
    });

    _builder.close();
    generateOk(rest::ResponseCode::OK, _builder.slice());

    return status;
  }

  std::string const& name = suffixes[0];
  // /_api/collection/<name>
  if (suffixes.size() == 1) {
    try {
      collectionRepresentation(name, /*showProperties*/ false,
                               /*showFigures*/ false, /*showCount*/ false,
                               /*detailedCount*/ false);
      generateOk(rest::ResponseCode::OK, _builder);
    } catch (basics::Exception const& ex) {  // do not log not found exceptions
      generateError(GeneralResponse::responseCode(ex.code()), ex.code(), ex.what());
    }
    return status;
  }

  if (suffixes.size() > 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expect GET /_api/collection/<collection-name>/<method>");
    return status;
  }

  std::string const& sub = suffixes[1];
  bool skipGenerate = false;
  _builder.clear();
  auto found = methods::Collections::lookup(  // find collection
      _vocbase,                               // vocbase to search
      name,                                   // collection name to find
      [&](std::shared_ptr<LogicalCollection> const& coll) -> void {  // callback if found
        TRI_ASSERT(coll);

        if (sub == "checksum") {
          // /_api/collection/<identifier>/checksum
          if (ServerState::instance()->isCoordinator()) {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
          }

          bool withRevisions = _request->parsedValue("withRevisions", false);
          bool withData = _request->parsedValue("withData", false);
          
          uint64_t checksum;
          TRI_voc_rid_t revId;
          auto res = methods::Collections::checksum(*coll, withRevisions, withData,
                                                    checksum, revId);
          
          if (res.ok()) {
            VPackObjectBuilder obj(&_builder, true);

            obj->add("checksum", VPackValue(std::to_string(checksum)));
            obj->add("revision", VPackValue(TRI_RidToString(revId)));

            // We do not need a transaction here
            methods::Collections::Context ctxt(_vocbase, *coll);

            collectionRepresentation(*coll,
                                     /*showProperties*/ false,
                                     /*showFigures*/ false,
                                     /*showCount*/ false,
                                     /*detailedCount*/ true);
          } else {
            skipGenerate = true;
            this->generateError(res);
          }
        } else if (sub == "figures") {
          // /_api/collection/<identifier>/figures
          _ctxt = std::make_unique<methods::Collections::Context>(_vocbase, *coll);
          skipGenerate = true;
          status = waitForFuture(
              collectionRepresentationAsync(*_ctxt,
                                            /*showProperties*/ true,
                                            /*showFigures*/ true,
                                            /*showCount*/ true,
                                            /*detailedCount*/ false)
                  .thenValue([this](futures::Unit&&) { standardResponse(); }));
        } else if (sub == "count") {
          // /_api/collection/<identifier>/count
          initializeTransaction(*coll);
          _ctxt = std::make_unique<methods::Collections::Context>(_vocbase, *coll,
                                                                  _activeTrx.get());
          skipGenerate = true;
          bool details = _request->parsedValue("details", false);
          status = waitForFuture(
              collectionRepresentationAsync(*_ctxt,
                                            /*showProperties*/ true,
                                            /*showFigures*/ false,
                                            /*showCount*/ true,
                                            /*detailedCount*/ details)
                  .thenValue([this](futures::Unit&&) { standardResponse(); }));
        } else if (sub == "properties") {
          // /_api/collection/<identifier>/properties
          collectionRepresentation(*coll,
                                   /*showProperties*/ true,
                                   /*showFigures*/ false,
                                   /*showCount*/ false,
                                   /*detailedCount*/ true);
        } else if (sub == "revision") {
          // /_api/collection/<identifier>/revision
          skipGenerate = true;
          _ctxt = std::make_unique<methods::Collections::Context>(_vocbase, *coll);
          status = waitForFuture(methods::Collections::revisionId(*_ctxt).thenValue(
              [this, coll](OperationResult&& res) {
                if (res.fail()) {
                  generateTransactionError(res);
                  return;
                }

                TRI_voc_rid_t rid = res.slice().isNumber()
                                        ? res.slice().getNumber<TRI_voc_rid_t>()
                                        : 0;
                {
                  VPackObjectBuilder obj(&_builder, true);
                  obj->add("revision", VPackValue(StringUtils::itoa(rid)));

                  // no need to use async variant
                  collectionRepresentation(*_ctxt, /*showProperties*/ true,
                                           /*showFigures*/ false, /*showCount*/ false,
                                           /*detailedCount*/ true);
                }

                standardResponse();
              }));
        } else if (sub == "shards") {
          // /_api/collection/<identifier>/shards
          if (!ServerState::instance()->isRunningInCluster()) {
            skipGenerate = true;  // must be internal error for tests
            this->generateError(Result(TRI_ERROR_INTERNAL));
            return;
          }

          VPackObjectBuilder obj(&_builder, true);  // need to open object

          collectionRepresentation(*coll,
                                   /*showProperties*/ true,
                                   /*showFigures*/ false,
                                   /*showCount*/ false,
                                   /*detailedCount*/ true);

          auto& ci = server().getFeature<ClusterFeature>().clusterInfo();
          auto shards = ci.getShardList(std::to_string(coll->planId()));

          if (_request->parsedValue("details", false)) {
            // with details
            VPackObjectBuilder arr(&_builder, "shards", true);
            for (ShardID const& shard : *shards) {
              std::vector<ServerID> servers;
              ci.getShardServers(shard, servers);

              if (servers.empty()) {
                continue;
              }

              VPackArrayBuilder arr(&_builder, shard);

              for (auto const& server : servers) {
                arr->add(VPackValue(server));
              }
            }
          } else {
            // no details
            VPackArrayBuilder arr(&_builder, "shards", true);

            for (ShardID const& shard : *shards) {
              arr->add(VPackValue(shard));
            }
          }

        } else {
          skipGenerate = true;
          this->generateError(
              rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
              "expecting one of the resources 'checksum', 'count', "
              "'figures', 'properties', 'responsibleShard', 'revision', "
              "'shards'");
        }
      });

  if (skipGenerate) {
    return status;
  }
  if (found.ok()) {
    standardResponse();
  } else {
    generateError(found);
  }

  return status;
}

// create a collection
void RestCollectionHandler::handleCommandPost() {
  bool parseSuccess = false;
  VPackSlice const body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    // error message generated in parseVPackBody
    events::CreateCollection(_vocbase.name(), "", TRI_ERROR_BAD_PARAMETER);
    return;
  }

  VPackSlice nameSlice;
  if (!body.isObject() || !(nameSlice = body.get("name")).isString() ||
      nameSlice.getStringLength() == 0) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_ARANGO_ILLEGAL_NAME);
    events::CreateCollection(_vocbase.name(), "", TRI_ERROR_ARANGO_ILLEGAL_NAME);
    return;
  }

  auto& cluster = _vocbase.server().getFeature<ClusterFeature>();
  bool waitForSyncReplication =
      _request->parsedValue("waitForSyncReplication",
                            cluster.createWaitsForSyncReplication());

  bool enforceReplicationFactor =
      _request->parsedValue("enforceReplicationFactor", true);

  TRI_col_type_e type = TRI_col_type_e::TRI_COL_TYPE_DOCUMENT;
  VPackSlice typeSlice = body.get("type");
  if (typeSlice.isString()) {
    if (typeSlice.compareString("edge") == 0 ||
        typeSlice.compareString("3") == 0) {
      type = TRI_col_type_e::TRI_COL_TYPE_EDGE;
    }
  } else if (typeSlice.isNumber()) {
    uint32_t t = typeSlice.getNumber<uint32_t>();
    if (t == TRI_col_type_e::TRI_COL_TYPE_EDGE) {
      type = TRI_col_type_e::TRI_COL_TYPE_EDGE;
    }
  }

  // for some "security" a whitelist of allowed parameters
  VPackBuilder filtered = VPackCollection::keep(
      body, std::unordered_set<std::string>{"doCompact",
                                            StaticStrings::DataSourceSystem,
                                            StaticStrings::DataSourceId,
                                            "isVolatile",
                                            StaticStrings::JournalSize,
                                            StaticStrings::IndexBuckets,
                                            "keyOptions",
                                            StaticStrings::WaitForSyncString,
                                            StaticStrings::CacheEnabled,
                                            StaticStrings::ShardKeys,
                                            StaticStrings::NumberOfShards,
                                            StaticStrings::DistributeShardsLike,
                                            "avoidServers",
                                            StaticStrings::IsSmart,
                                            StaticStrings::ShardingStrategy,
                                            StaticStrings::GraphSmartGraphAttribute,
                                            StaticStrings::SmartJoinAttribute,
                                            StaticStrings::ReplicationFactor,
                                            StaticStrings::MinReplicationFactor,
                                            "servers"
                                          });
  VPackSlice const parameters = filtered.slice();

  // now we can create the collection
  std::string const& name = nameSlice.copyString();
  _builder.clear();
  Result res = methods::Collections::create(
      _vocbase,                  // collection vocbase
      name,                      // colection name
      type,                      // collection type
      parameters,                // collection properties
      waitForSyncReplication,    // replication wait flag
      enforceReplicationFactor,  // replication factor flag
      false,       // new Database?, here always false
      [&](std::shared_ptr<LogicalCollection> const& coll) -> void {
        TRI_ASSERT(coll);
        collectionRepresentation(coll->name(),
                                 /*showProperties*/ true,
                                 /*showFigures*/ false,
                                 /*showCount*/ false,
                                 /*detailedCount*/ true);
      });

  if (res.ok()) {
    generateOk(rest::ResponseCode::OK, _builder);
  } else {
    generateError(res);
  }
}

RestStatus RestCollectionHandler::handleCommandPut() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();
  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected PUT /_api/collection/<collection-name>/<action>");
    return RestStatus::DONE;
  }
  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    // error message generated in parseVPackBody
    return RestStatus::DONE;
  }

  std::string const& name = suffixes[0];
  std::string const& sub = suffixes[1];

  if (sub != "responsibleShard" && !body.isObject()) {
    // if the caller has sent an empty body. for convenience, let's turn this into an object
    // however, for the "responsibleShard" case we want to distinguish between string values,
    // object values etc. - so we don't do the conversion here
    body = VPackSlice::emptyObjectSlice();
  }

  Result res;
  _builder.clear();
  RestStatus status = RestStatus::DONE;
  bool generateResponse = true;
  auto found = methods::Collections::lookup(  // find collection
      _vocbase,                               // vocbase to search
      name,                                   // collection name to find
      [&](std::shared_ptr<LogicalCollection> const& coll) -> void {  // callback if found
        TRI_ASSERT(coll);

        if (sub == "load") {
          res = methods::Collections::load(_vocbase, coll.get());

          if (res.ok()) {
            bool cc = VelocyPackHelper::getBooleanValue(body, "count", true);
            collectionRepresentation(name, /*showProperties*/ false,
                                     /*showFigures*/ false, /*showCount*/ cc,
                                     /*detailedCount*/ false);
          }
        } else if (sub == "unload") {
          bool flush = _request->parsedValue("flush", false);

          if (flush && TRI_vocbase_col_status_e::TRI_VOC_COL_STATUS_LOADED ==
                           coll->status()) {
            EngineSelectorFeature::ENGINE->flushWal(false, false, false);
          }

          res = methods::Collections::unload(&_vocbase, coll.get());

          if (res.ok()) {
            collectionRepresentation(name, /*showProperties*/ false,
                                     /*showFigures*/ false, /*showCount*/ false,
                                     /*detailedCount*/ true);
          }
        } else if (sub == "compact") {
          res = coll->compact();

          if (res.ok()) {
            collectionRepresentation(name, /*showProperties*/ false,
                                     /*showFigures*/ false, /*showCount*/ false,
                                     /*detailedCount*/ true);
          }
        } else if (sub == "responsibleShard") {
          if (!ServerState::instance()->isCoordinator()) {
            res.reset(TRI_ERROR_CLUSTER_ONLY_ON_COORDINATOR);
          } else {
            VPackBuilder temp;
            if (body.isString()) {
              temp.openObject();
              temp.add(StaticStrings::KeyString, body);
              temp.close();
              body = temp.slice();
            } else if (body.isNumber()) {
              temp.openObject();
              temp.add(StaticStrings::KeyString, VPackValue(std::to_string(body.getNumber<int64_t>())));
              temp.close();
              body = temp.slice();
            }
            if (!body.isObject()) {
              THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, "expecting object for responsibleShard");
            }

            std::string shardId;
            res = coll->getResponsibleShard(body, false, shardId);

            if (res.ok()) {
              _builder.openObject();
              _builder.add("shardId", VPackValue(shardId));
              _builder.close();
            }
          }
        } else if (sub == "truncate") {
          {
            OperationOptions opts;

            opts.waitForSync = _request->parsedValue("waitForSync", false);
            opts.isSynchronousReplicationFrom =
                _request->value("isSynchronousReplication");

            _activeTrx = createTransaction(coll->name(), AccessMode::Type::EXCLUSIVE);
            _activeTrx->addHint(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
            _activeTrx->addHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE);
            res = _activeTrx->begin();

            if (res.ok()) {
              generateResponse = false;
              status = waitForFuture(
                  _activeTrx->truncateAsync(coll->name(), opts).thenValue([this, coll](OperationResult&& opres) {
                    // Will commit if no error occured.
                    // or abort if an error occured.
                    // result stays valid!
                    Result res = _activeTrx->finish(opres.result);
                    if (opres.fail()) {
                      generateTransactionError(opres);
                      return;
                    }

                    if (res.fail()) {
                      generateTransactionError(coll->name(), res, "");
                      return;
                    }

                    _activeTrx.reset();

                    // wait for the transaction to finish first. only after that compact the
                    // data range(s) for the collection
                    // we shouldn't run compact() as part of the transaction, because the compact
                    // will be useless inside due to the snapshot the transaction has taken
                    coll->compact();

                    if (ServerState::instance()->isCoordinator()) {  // ClusterInfo::loadPlan eventually
                                                                     // updates status
                      coll->setStatus(TRI_vocbase_col_status_e::TRI_VOC_COL_STATUS_LOADED);
                    }

                    // no need to use async method, no
                    collectionRepresentation(*coll,
                                             /*showProperties*/ false,
                                             /*showFigures*/ false,
                                             /*showCount*/ false,
                                             /*detailedCount*/ true);
                    standardResponse();
                  }));
            }
          }
        } else if (sub == "properties") {
          // replication checks
          if (body.get(StaticStrings::ReplicationFactor).isNumber() &&
              body.get(StaticStrings::ReplicationFactor).getInt() > 0) {
            uint64_t replicationFactor =
                body.get(StaticStrings::ReplicationFactor).getUInt();
            if (ServerState::instance()->isRunningInCluster() &&
                replicationFactor > server()
                                        .getFeature<ClusterFeature>()
                                        .clusterInfo()
                                        .getCurrentDBServers()
                                        .size()) {
              THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_INSUFFICIENT_DBSERVERS);
            }
          }

          // write concern checks
          // not an error: for historical reasons the write concern is read from the
          // variable "minReplicationFactor"
          if (body.get(StaticStrings::MinReplicationFactor).isNumber() &&
              body.get(StaticStrings::MinReplicationFactor).getInt() > 0) {
            uint64_t writeConcern =
                body.get(StaticStrings::MinReplicationFactor).getUInt();
            if (ServerState::instance()->isRunningInCluster() &&
                writeConcern > server().getFeature<ClusterFeature>()
                                           .clusterInfo()
                                           .getCurrentDBServers()
                                           .size()) {
              THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_INSUFFICIENT_DBSERVERS);
            }
          }

          std::vector<std::string> keep = {StaticStrings::DoCompact,
                                           StaticStrings::JournalSize,
                                           StaticStrings::WaitForSyncString,
                                           StaticStrings::IndexBuckets,
                                           StaticStrings::ReplicationFactor,
                                           StaticStrings::MinReplicationFactor,
                                           StaticStrings::CacheEnabled};
          VPackBuilder props = VPackCollection::keep(body, keep);

          res = methods::Collections::updateProperties(*coll, props.slice(), false  // always a full-update
          );

          if (res.ok()) {
            collectionRepresentation(name, /*showProperties*/ true,
                                     /*showFigures*/ false, /*showCount*/ false,
                                     /*detailedCount*/ true);
          }

        } else if (sub == "rename") {
          VPackSlice const newNameSlice = body.get("name");

          if (!newNameSlice.isString()) {
            res = Result(TRI_ERROR_ARANGO_ILLEGAL_NAME, "name is empty");
            return;
          }

          std::string const newName = newNameSlice.copyString();

          res = methods::Collections::rename(*coll, newName, false);

          if (res.ok()) {
            collectionRepresentation(newName, /*showProperties*/ false,
                                     /*showFigures*/ false, /*showCount*/ false,
                                     /*detailedCount*/ true);
          }
        } else if (sub == "loadIndexesIntoMemory") {
          generateResponse = false;
          status = waitForFuture(
              methods::Collections::warmup(_vocbase, *coll).thenValue([this, coll](Result&& res) {
                if (res.fail()) {
                  generateTransactionError(coll->name(), res, "");
                  return;
                }

                {
                  VPackObjectBuilder obj(&_builder, true);
                  obj->add("result", VPackValue(res.ok()));
                }

                standardResponse();
              }));
        } else {
          res = handleExtraCommandPut(*coll, sub, _builder);
          if (res.is(TRI_ERROR_NOT_IMPLEMENTED)) {
            res.reset(
                TRI_ERROR_HTTP_NOT_FOUND,
                "expecting one of the actions 'load', 'unload', 'truncate',"
                " 'properties', 'compact', 'rename', 'loadIndexesIntoMemory'");
          }
        }
      });

  if (generateResponse) {
    if (found.fail()) {
      generateError(found);
    } else if (res.ok()) {
      standardResponse();
    } else {
      generateError(res);
    }
  }

  return status;
}

void RestCollectionHandler::handleCommandDelete() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  if (suffixes.size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected DELETE /_api/collection/<collection-name>");
    events::DropCollection(_vocbase.name(), "", TRI_ERROR_HTTP_BAD_PARAMETER);
    return;
  }

  std::string const& name = suffixes[0];
  bool allowDropSystem = _request->parsedValue("isSystem", false);
  _builder.clear();
  Result res;
  Result found = methods::Collections::lookup(
      _vocbase,  // vocbase to search
      name,      // collection name to find
      [&](std::shared_ptr<LogicalCollection> const& coll) -> void {  // callback if found
        TRI_ASSERT(coll);

        auto cid = std::to_string(coll->id());
        VPackObjectBuilder obj(&_builder, true);

        obj->add("id", VPackValue(cid));
        res = methods::Collections::drop(*coll, allowDropSystem, -1.0);
      });

  if (found.fail()) {
    events::DropCollection(_vocbase.name(), name, found.errorNumber());
    generateError(found);
  } else if (res.fail()) {
    generateError(res);
  } else {
    generateOk(rest::ResponseCode::OK, _builder);
  }
}

/// generate collection info. We lookup the collection again, because in the
/// cluster someinfo is lazily added in loadPlan, which means load, unload,
/// truncate
/// and create will not immediately show the expected results on a collection
/// object.
void RestCollectionHandler::collectionRepresentation(std::string const& name,
                                                     bool showProperties, bool showFigures,
                                                     bool showCount, bool detailedCount) {
  Result r = methods::Collections::lookup(
      _vocbase,  // vocbase to search
      name,      // collection to find
      [=](std::shared_ptr<LogicalCollection> const& coll) -> void {  // callback if found
        TRI_ASSERT(coll);
        collectionRepresentation(*coll, showProperties, showFigures, showCount, detailedCount);
      });

  if (r.fail()) {
    THROW_ARANGO_EXCEPTION(r);
  }
}

void RestCollectionHandler::collectionRepresentation(LogicalCollection& coll,
                                                     bool showProperties, bool showFigures,
                                                     bool showCount, bool detailedCount) {
  if (showProperties || showCount) {
    // Here we need a transaction
    initializeTransaction(coll);
    methods::Collections::Context ctxt(_vocbase, coll, _activeTrx.get());

    collectionRepresentation(ctxt, showProperties, showFigures, showCount, detailedCount);
  } else {
    // We do not need a transaction here
    methods::Collections::Context ctxt(_vocbase, coll);

    collectionRepresentation(ctxt, showProperties, showFigures, showCount, detailedCount);
  }
}

void RestCollectionHandler::collectionRepresentation(methods::Collections::Context& ctxt,
                                                     bool showProperties, bool showFigures,
                                                     bool showCount, bool detailedCount) {
  collectionRepresentationAsync(ctxt, showProperties, showFigures, showCount, detailedCount)
      .get();
}

futures::Future<futures::Unit> RestCollectionHandler::collectionRepresentationAsync(
    methods::Collections::Context& ctxt, bool showProperties, bool showFigures,
    bool showCount, bool detailedCount) {
  bool wasOpen = _builder.isOpenObject();
  if (!wasOpen) {
    _builder.openObject();
  }

  auto coll = ctxt.coll();
  TRI_ASSERT(coll != nullptr);

  // `methods::Collections::properties` will filter these out
  _builder.add(StaticStrings::DataSourceId, VPackValue(std::to_string(coll->id())));
  _builder.add(StaticStrings::DataSourceName, VPackValue(coll->name()));
  _builder.add("status", VPackValue(coll->status()));
  _builder.add(StaticStrings::DataSourceType, VPackValue(coll->type()));

  if (!showProperties) {
    _builder.add(StaticStrings::DataSourceSystem, VPackValue(coll->system()));
    _builder.add(StaticStrings::DataSourceGuid, VPackValue(coll->guid()));
  } else {
    Result res = methods::Collections::properties(ctxt, _builder);
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
  }

  futures::Future<OperationResult> figures = futures::makeFuture(OperationResult());
  if (showFigures) {
    figures = coll->figures();
  }

  return std::move(figures)
      .thenValue([=, &ctxt](OperationResult&& figures) -> futures::Future<OperationResult> {
        if (figures.buffer) {
          _builder.add("figures", figures.slice());
        }

        if (showCount) {
          auto trx = ctxt.trx(AccessMode::Type::READ, true, true);
          TRI_ASSERT(trx != nullptr);
          return trx->countAsync(coll->name(),
                                 detailedCount ? transaction::CountType::Detailed
                                               : transaction::CountType::Normal);
        }
        return futures::makeFuture(OperationResult());
      })
      .thenValue([=, &ctxt](OperationResult&& opRes) -> void {
        if (opRes.fail()) {
          if (showCount) {
            auto trx = ctxt.trx(AccessMode::Type::READ, true, true);
            TRI_ASSERT(trx != nullptr);
            trx->finish(opRes.result);
          }
          THROW_ARANGO_EXCEPTION(opRes.result);
        }

        if (showCount) {
          _builder.add("count", opRes.slice());
        }

        if (!wasOpen) {
          _builder.close();
        }
      });
}

void RestCollectionHandler::standardResponse() {
  generateOk(rest::ResponseCode::OK, _builder);
  _response->setHeaderNC(StaticStrings::Location, _request->requestPath());
}

void RestCollectionHandler::initializeTransaction(LogicalCollection& coll) {
  try {
    _activeTrx = createTransaction(coll.name(), AccessMode::Type::READ);
  } catch (basics::Exception const& ex) {
    if (ex.code() == TRI_ERROR_TRANSACTION_NOT_FOUND) {
      // this will happen if the tid of a managed transaction is passed in,
      // but the transaction hasn't yet started on the DB server. in
      // this case, we create an ad-hoc transaction on the underlying
      // collection
      _activeTrx = std::make_unique<SingleCollectionTransaction>(
          transaction::StandaloneContext::Create(_vocbase), coll.name(),
          AccessMode::Type::READ);
    } else {
      throw;
    }
  }

  TRI_ASSERT(_activeTrx != nullptr);
  Result res = _activeTrx->begin();

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }
}
