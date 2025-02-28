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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_VOC_BASE_API_COLLECTIONS_H
#define ARANGOD_VOC_BASE_API_COLLECTIONS_H 1

#include "Basics/Result.h"
#include "Futures/Future.h"
#include "Utils/OperationResult.h"
#include "VocBase/AccessMode.h"
#include "VocBase/voc-types.h"
#include "VocBase/vocbase.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>
#include <functional>

namespace arangodb {
class ClusterFeature;
class LogicalCollection;
struct CollectionCreationInfo;

namespace transaction {
class Methods;
}

namespace methods {

/// Common code for collection REST handler and v8-collections
struct Collections {
  struct Context {
    Context(Context const&) = delete;
    Context& operator=(Context const&) = delete;

    Context(TRI_vocbase_t& vocbase, LogicalCollection& coll);
    Context(TRI_vocbase_t& vocbase, LogicalCollection& coll, transaction::Methods* trx);

    ~Context();

    transaction::Methods* trx(AccessMode::Type const& type, bool embeddable,
                              bool forceLoadCollection);
    TRI_vocbase_t& vocbase() const;
    LogicalCollection* coll() const;

   private:
    TRI_vocbase_t& _vocbase;
    LogicalCollection& _coll;
    transaction::Methods* _trx;
    bool const _responsibleForTrx;
  };

  typedef std::function<void(std::shared_ptr<LogicalCollection> const&)> FuncCallback;
  typedef std::function<void(std::vector<std::shared_ptr<LogicalCollection>> const&)> MultiFuncCallback;
  typedef std::function<void(velocypack::Slice const&)> DocCallback;

  static void enumerate(TRI_vocbase_t* vocbase, FuncCallback const&);

  /// @brief lookup a collection in vocbase or clusterinfo.
  static arangodb::Result lookup(    // find collection
      TRI_vocbase_t const& vocbase,  // vocbase to search
      std::string const& name,       // collection name
      FuncCallback callback          // invoke on found collection
  );

  /// Create collection, ownership of collection in callback is
  /// transferred to callee
  static arangodb::Result create(                     // create collection
      TRI_vocbase_t& vocbase,                         // collection vocbase
      std::string const& name,                        // collection name
      TRI_col_type_e collectionType,                  // collection type
      arangodb::velocypack::Slice const& properties,  // collection properties
      bool createWaitsForSyncReplication,             // replication wait flag
      bool enforceReplicationFactor,                  // replication factor flag
      bool isNewDatabase,
      FuncCallback callback);  // invoke on collection creation

  /// Create many collections, ownership of collections in callback is
  /// transferred to callee
  static Result create(TRI_vocbase_t&, std::vector<CollectionCreationInfo> const& infos,
                       bool createWaitsForSyncReplication,
                       bool enforceReplicationFactor, bool isNewDatabase,
                       std::shared_ptr<LogicalCollection> const& colPtr,
                       MultiFuncCallback const&);

  static std::pair<Result, std::shared_ptr<LogicalCollection>> createSystem(
      TRI_vocbase_t& vocbase, std::string const& name, bool isNewDatabase);
  static void createSystemCollectionProperties(std::string const& collectionName,
                                               VPackBuilder& builder, TRI_vocbase_t const&);

  static Result load(TRI_vocbase_t& vocbase, LogicalCollection* coll);
  static Result unload(TRI_vocbase_t* vocbase, LogicalCollection* coll);

  static Result properties(Context& ctxt, velocypack::Builder&);
  static Result updateProperties(LogicalCollection& collection,
                                 velocypack::Slice const& props, bool partialUpdate);

  static Result rename(LogicalCollection& collection,
                       std::string const& newName, bool doOverride);

  static arangodb::Result drop(           // drop collection
      arangodb::LogicalCollection& coll,  // collection to drop
      bool allowDropSystem,               // allow dropping system collection
      double timeout                      // single-server drop timeout
  );

  static futures::Future<Result> warmup(TRI_vocbase_t& vocbase,
                                        LogicalCollection const& coll);

  static futures::Future<OperationResult> revisionId(Context& ctxt);

  /// @brief Helper implementation similar to ArangoCollection.all() in v8
  static arangodb::Result all(TRI_vocbase_t& vocbase, std::string const& cname,
                              DocCallback const& cb);
  
  static arangodb::Result checksum(LogicalCollection& collection,
                                   bool withRevisions, bool withData,
                                   uint64_t& checksum, TRI_voc_rid_t& revId);
};
#ifdef USE_ENTERPRISE
Result ULColCoordinatorEnterprise(ClusterFeature& feature, std::string const& databaseName,
                                  std::string const& collectionCID,
                                  TRI_vocbase_col_status_e status);

Result DropColCoordinatorEnterprise(LogicalCollection* collection, bool allowDropSystem);
#endif
}  // namespace methods
}  // namespace arangodb

#endif
