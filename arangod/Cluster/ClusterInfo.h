////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Max Neunhoeffer
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_CLUSTER_CLUSTER_INFO_H
#define ARANGOD_CLUSTER_CLUSTER_INFO_H 1

#include "Basics/Common.h"

#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

#include "Agency/AgencyComm.h"
#include "Basics/Mutex.h"
#include "Basics/ReadLocker.h"
#include "Basics/ReadWriteLock.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/AgencyCallbackRegistry.h"
#include "Cluster/ClusterTypes.h"
#include "Cluster/RebootTracker.h"
#include "VocBase/voc-types.h"
#include "VocBase/vocbase.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/VocbaseInfo.h"

namespace arangodb {
namespace velocypack {
class Slice;
}

class ClusterInfo;
class LogicalCollection;
struct ClusterCollectionCreationInfo;

// make sure a collection is still in Plan
// we are only going from *assuming* that it is present
// to it being changed to not present.
class CollectionWatcher
{
public:
  CollectionWatcher(CollectionWatcher const&) = delete;
  CollectionWatcher(AgencyCallbackRegistry *agencyCallbackRegistry, LogicalCollection const& collection)
    : _agencyCallbackRegistry(agencyCallbackRegistry), _present(true) {
    AgencyComm ac;

    std::string databaseName = collection.vocbase().name();
    std::string collectionID = std::to_string(collection.id());
    std::string where = "Plan/Collections/" + databaseName + "/" + collectionID;

    _agencyCallback = std::make_shared<AgencyCallback>(
        ac, where,
        [this](VPackSlice const& result) {
          if (result.isNone()) {
            _present.store(false);
          }
          return true;
        },
        true, false);
    _agencyCallbackRegistry->registerCallback(_agencyCallback);
  };
  ~CollectionWatcher();

  bool isPresent() {
    // Make sure we did not miss a callback
    _agencyCallback->refetchAndUpdate(true, false);
    return _present.load();
  };

private:
  AgencyCallbackRegistry *_agencyCallbackRegistry;
  std::shared_ptr<AgencyCallback> _agencyCallback;

  // TODO: this does not really need to be atomic: We only write to it
  //       in the callback, and we only read it in `isPresent`; it does
  //       not actually matter whether this value is "correct".
  std::atomic<bool> _present;
};

// Read the collection from Plan; this is an object to have a valid VPack
// around to read from and to not have to carry around vpack builders.
// Might want to do the error handling with throw/catch?
class PlanCollectionReader {
 public:
  PlanCollectionReader(PlanCollectionReader const&&) = delete;
  PlanCollectionReader(PlanCollectionReader const&) = delete;
  explicit PlanCollectionReader(LogicalCollection const& collection) {
    std::string databaseName = collection.vocbase().name();
    std::string collectionID = std::to_string(collection.id());

    AgencyComm ac;

    std::string path =
        "Plan/Collections/" + databaseName + "/" + collectionID;
    _read = ac.getValues(path);

    if (!_read.successful()) {
      _state = Result(TRI_ERROR_CLUSTER_READING_PLAN_AGENCY,
                      "Could not retrieve " + path + " from agency");
      return;
    }

    _collection = _read.slice()[0].get(std::vector<std::string>(
        {AgencyCommManager::path(), "Plan", "Collections", databaseName, collectionID}));

    if (!_collection.isObject()) {
      _state = Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
      return;
    }
    _state = Result();
  }
  VPackSlice indexes();
  VPackSlice slice() { return _collection; }
  Result state() { return _state; }

 private:
  AgencyCommResult _read;
  Result _state;
  velocypack::Slice _collection;
};

class CollectionInfoCurrent {
  friend class ClusterInfo;

 public:
  explicit CollectionInfoCurrent(uint64_t currentVersion);

  CollectionInfoCurrent(CollectionInfoCurrent const&) = delete;

  CollectionInfoCurrent(CollectionInfoCurrent&&) = delete;

  CollectionInfoCurrent& operator=(CollectionInfoCurrent const&) = delete;

  CollectionInfoCurrent& operator=(CollectionInfoCurrent&&) = delete;

  virtual ~CollectionInfoCurrent();

 public:
  bool add(ShardID const& shardID, VPackSlice slice) {
    auto it = _vpacks.find(shardID);
    if (it == _vpacks.end()) {
      auto builder = std::make_shared<VPackBuilder>();
      builder->add(slice);
      _vpacks.insert(std::make_pair(shardID, builder));
      return true;
    }
    return false;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the indexes
  //////////////////////////////////////////////////////////////////////////////

  VPackSlice const getIndexes(ShardID const& shardID) const {
    auto it = _vpacks.find(shardID);
    if (it != _vpacks.end()) {
      VPackSlice slice = it->second->slice();
      return slice.get("indexes");
    }
    return VPackSlice::noneSlice();
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the error flag for a shardID
  //////////////////////////////////////////////////////////////////////////////

  bool error(ShardID const& shardID) const { return getFlag(StaticStrings::Error, shardID); }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the error flag for all shardIDs
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_map<ShardID, bool> error() const { return getFlag(StaticStrings::Error); }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the errorNum for one shardID
  //////////////////////////////////////////////////////////////////////////////

  int errorNum(ShardID const& shardID) const {
    auto it = _vpacks.find(shardID);
    if (it != _vpacks.end()) {
      VPackSlice slice = it->second->slice();
      return arangodb::basics::VelocyPackHelper::getNumericValue<int>(
          slice, StaticStrings::ErrorNum, 0);
    }
    return 0;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the errorNum for all shardIDs
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_map<ShardID, int> errorNum() const {
    std::unordered_map<ShardID, int> m;

    for (auto const& it : _vpacks) {
      int s =
          arangodb::basics::VelocyPackHelper::getNumericValue<int>(it.second->slice(),
                                                                   StaticStrings::ErrorNum, 0);
      m.insert(std::make_pair(it.first, s));
    }
    return m;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the current leader and followers for a shard
  //////////////////////////////////////////////////////////////////////////////

  TEST_VIRTUAL std::vector<ServerID> servers(ShardID const& shardID) const {
    std::vector<ServerID> v;

    auto it = _vpacks.find(shardID);
    if (it != _vpacks.end()) {
      VPackSlice slice = it->second->slice();

      VPackSlice servers = slice.get("servers");
      if (servers.isArray()) {
        for (VPackSlice server : VPackArrayIterator(servers)) {
          if (server.isString()) {
            v.push_back(server.copyString());
          }
        }
      }
    }
    return v;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the current failover candidates for the given shard
  //////////////////////////////////////////////////////////////////////////////

  TEST_VIRTUAL std::vector<ServerID> failoverCandidates(ShardID const& shardID) const {
    std::vector<ServerID> v;

    auto it = _vpacks.find(shardID);
    if (it != _vpacks.end()) {
      VPackSlice slice = it->second->slice();

      VPackSlice servers = slice.get(StaticStrings::FailoverCandidates);
      if (servers.isArray()) {
        for (VPackSlice server : VPackArrayIterator(servers)) {
          TRI_ASSERT(server.isString());
          if (server.isString()) {
            v.push_back(server.copyString());
          }
        }
      }
    }
    return v;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the errorMessage entry for one shardID
  //////////////////////////////////////////////////////////////////////////////

  std::string errorMessage(ShardID const& shardID) const {
    auto it = _vpacks.find(shardID);
    if (it != _vpacks.end()) {
      VPackSlice slice = it->second->slice();
      if (slice.isObject() && slice.hasKey(StaticStrings::ErrorMessage)) {
        return slice.get(StaticStrings::ErrorMessage).copyString();
      }
    }
    return std::string();
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get version that underlies this info in Current in the agency
  //////////////////////////////////////////////////////////////////////////////

  uint64_t getCurrentVersion() const { return _currentVersion; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief local helper to return boolean flags
  //////////////////////////////////////////////////////////////////////////////

 private:
  template <typename T>
  bool getFlag(T const& name, ShardID const& shardID) const {
    auto it = _vpacks.find(shardID);
    if (it != _vpacks.end()) {
      return arangodb::basics::VelocyPackHelper::getBooleanValue(it->second->slice(),
                                                                 name, false);
    }
    return false;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief local helper to return a map to boolean
  //////////////////////////////////////////////////////////////////////////////

  template <typename T>
  std::unordered_map<ShardID, bool> getFlag(T const& name) const {
    std::unordered_map<ShardID, bool> m;
    for (auto const& it : _vpacks) {
      auto vpack = it.second;
      bool b = arangodb::basics::VelocyPackHelper::getBooleanValue(vpack->slice(),
                                                                   name, false);
      m.emplace(it.first, b);
    }
    return m;
  }

 private:
  std::unordered_map<ShardID, std::shared_ptr<VPackBuilder>> _vpacks;

  uint64_t _currentVersion;  // Version of Current in the agency that
                             // underpins the data presented in this object
};

#ifdef ARANGODB_USE_GOOGLE_TESTS
class ClusterInfo {
#else
class ClusterInfo final {
#endif
 private:
  typedef std::unordered_map<CollectionID, std::shared_ptr<LogicalCollection>> DatabaseCollections;
  typedef std::unordered_map<DatabaseID, DatabaseCollections> AllCollections;
  typedef std::unordered_map<CollectionID, std::shared_ptr<CollectionInfoCurrent>> DatabaseCollectionsCurrent;
  typedef std::unordered_map<DatabaseID, DatabaseCollectionsCurrent> AllCollectionsCurrent;

  typedef std::unordered_map<ViewID, std::shared_ptr<LogicalView>> DatabaseViews;
  typedef std::unordered_map<DatabaseID, DatabaseViews> AllViews;

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief initializes library
  /// We are a singleton class, therefore nobody is allowed to create
  /// new instances or copy them, except we ourselves.
  //////////////////////////////////////////////////////////////////////////////

  ClusterInfo(ClusterInfo const&) = delete;             // not implemented
  ClusterInfo& operator=(ClusterInfo const&) = delete;  // not implemented

 public:
  class ServersKnown {
   public:
    ServersKnown() = default;
    ServersKnown(VPackSlice serversKnownSlice, std::unordered_set<ServerID> const& servers);

    class KnownServer {
     public:
      explicit constexpr KnownServer(RebootId rebootId) : _rebootId(rebootId) {}

      RebootId rebootId() const { return _rebootId; }

     private:
      RebootId _rebootId;
    };

    std::unordered_map<ServerID, KnownServer> const& serversKnown() const noexcept;

    std::unordered_map<ServerID, RebootId> rebootIds() const;

   private:
    std::unordered_map<ServerID, KnownServer> _serversKnown;
  };

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief creates library
  //////////////////////////////////////////////////////////////////////////////

  explicit ClusterInfo(application_features::ApplicationServer&, AgencyCallbackRegistry*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief shuts down library
  //////////////////////////////////////////////////////////////////////////////

  TEST_VIRTUAL ~ClusterInfo();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief cleanup method which frees cluster-internal shared ptrs on shutdown
  //////////////////////////////////////////////////////////////////////////////

  void cleanup();

  /// @brief produces an agency dump and logs it
  void logAgencyDump() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get a number of cluster-wide unique IDs, returns the first
  /// one and guarantees that <number> are reserved for the caller.
  //////////////////////////////////////////////////////////////////////////////

  uint64_t uniqid(uint64_t = 1);

  /**
   * @brief Agency dump including replicated log and compaction
   * @param  body  Builder to fill with dump
   * @return       Operation's result
   */
  arangodb::Result agencyDump(std::shared_ptr<VPackBuilder> body);

  /**
   * @brief Agency plan
   * @param  body  Builder to fill with copy of plan
   * @return       Operation's result
   */
  arangodb::Result agencyPlan(std::shared_ptr<VPackBuilder> body);

  /**
   * @brief Overwrite agency plan
   * @param  plan  Plan to adapt to
   * @return       Operation's result
   */
  arangodb::Result agencyReplan(VPackSlice const plan);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief flush the caches (used for testing only)
  //////////////////////////////////////////////////////////////////////////////

  void flush();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ask whether a cluster database exists
  //////////////////////////////////////////////////////////////////////////////

  bool doesDatabaseExist(DatabaseID const&, bool = false);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get list of databases in the cluster
  //////////////////////////////////////////////////////////////////////////////

  std::vector<DatabaseID> databases(bool = false);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief (re-)load the information about our plan
  /// Usually one does not have to call this directly.
  //////////////////////////////////////////////////////////////////////////////

  void loadPlan();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief (re-)load the information about current state
  /// Usually one does not have to call this directly.
  //////////////////////////////////////////////////////////////////////////////

  void loadCurrent();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ask about a collection
  /// Throwing version, deprecated.
  //////////////////////////////////////////////////////////////////////////////

  TEST_VIRTUAL std::shared_ptr<LogicalCollection> getCollection(DatabaseID const&,
                                                                CollectionID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ask about a collection
  /// If it is not found in the cache, the cache is reloaded once. The second
  /// argument can be a collection ID or a collection name (both cluster-wide).
  /// if the collection is not found afterwards, this method will throw an
  /// exception
  /// will not throw but return nullptr if the collection isn't found.
  //////////////////////////////////////////////////////////////////////////////

  TEST_VIRTUAL std::shared_ptr<LogicalCollection> getCollectionNT(DatabaseID const&,
                                                                  CollectionID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// Format error message for TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND
  //////////////////////////////////////////////////////////////////////////////
  static std::string getCollectionNotFoundMsg(DatabaseID const&, CollectionID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ask about all collections of a database
  //////////////////////////////////////////////////////////////////////////////

  TEST_VIRTUAL std::vector<std::shared_ptr<LogicalCollection>> const getCollections(DatabaseID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ask about a view
  /// If it is not found in the cache, the cache is reloaded once. The second
  /// argument can be a collection ID or a view name (both cluster-wide).
  //////////////////////////////////////////////////////////////////////////////

  std::shared_ptr<LogicalView> getView(DatabaseID const& vocbase, ViewID const& viewID);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ask about all views of a database
  //////////////////////////////////////////////////////////////////////////////

  std::vector<std::shared_ptr<LogicalView>> const getViews(DatabaseID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ask about a collection in current. This returns information about
  /// all shards in the collection.
  /// If it is not found in the cache, the cache is reloaded once.
  //////////////////////////////////////////////////////////////////////////////

  TEST_VIRTUAL std::shared_ptr<CollectionInfoCurrent> getCollectionCurrent(
      DatabaseID const&, CollectionID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Get the RebootTracker
  //////////////////////////////////////////////////////////////////////////////

  cluster::RebootTracker& rebootTracker() noexcept;
  cluster::RebootTracker const& rebootTracker() const noexcept;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief create database in coordinator
  ///
  /// A database is first created in the isBuilding state, and therefore not
  ///  visible or usable to the outside world.
  ///
  /// After the database has been fully setup, it is then confirmed created
  /// and becomes visible and usable.
  ///
  /// If any error happens on the way, a pending database will be cleaned up
  ///
  //////////////////////////////////////////////////////////////////////////////
  Result createIsBuildingDatabaseCoordinator(CreateDatabaseInfo const& database);
  Result createFinalizeDatabaseCoordinator(CreateDatabaseInfo const& database);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief removes database and collection entries within the agency plan
  //////////////////////////////////////////////////////////////////////////////
  Result cancelCreateDatabaseCoordinator(CreateDatabaseInfo const& database);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief drop database in coordinator
  //////////////////////////////////////////////////////////////////////////////
  Result dropDatabaseCoordinator(  // drop database
      std::string const& name,     // database name
      double timeout               // request timeout
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief create collection in coordinator
  //////////////////////////////////////////////////////////////////////////////
  Result createCollectionCoordinator(   // create collection
      std::string const& databaseName,  // database name
      std::string const& collectionID, uint64_t numberOfShards,
      uint64_t replicationFactor, uint64_t writeConcern,
      bool waitForReplication, arangodb::velocypack::Slice const& json,
      double timeout,  // request timeout
      bool isNewDatabase,
      std::shared_ptr<LogicalCollection> const& colToDistributeShardsLike);

  /// @brief this method does an atomic check of the preconditions for the
  /// collections to be created, using the currently loaded plan. it populates
  /// the plan version used for the checks
  Result checkCollectionPreconditions(std::string const& databaseName,
                                      std::vector<ClusterCollectionCreationInfo> const& infos,
                                      uint64_t& planVersion);

  /// @brief create multiple collections in coordinator
  ///        If any one of these collections fails, all creations will be
  ///        rolled back.
  /// Note that in contrast to most other methods here, this method does not
  /// get a timeout parameter, but an endTime parameter!!!
  Result createCollectionsCoordinator(std::string const& databaseName,
                                      std::vector<ClusterCollectionCreationInfo>&,
                                      double endTime, bool isNewDatabase,
                                      std::shared_ptr<LogicalCollection> const& colToDistributeShardsLike);

  /// @brief drop collection in coordinator
  //////////////////////////////////////////////////////////////////////////////
  Result dropCollectionCoordinator(     // drop collection
      std::string const& databaseName,  // database name
      std::string const& collectionID,  // collection identifier
      double timeout                    // request timeout
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set collection properties in coordinator
  //////////////////////////////////////////////////////////////////////////////

  Result setCollectionPropertiesCoordinator(std::string const& databaseName,
                                            std::string const& collectionID,
                                            LogicalCollection const*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set collection status in coordinator
  //////////////////////////////////////////////////////////////////////////////

  Result setCollectionStatusCoordinator(std::string const& databaseName,
                                        std::string const& collectionID,
                                        TRI_vocbase_col_status_e status);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief create view in coordinator
  //////////////////////////////////////////////////////////////////////////////
  Result createViewCoordinator(         // create view
      std::string const& databaseName,  // database name
      std::string const& viewID,        // view identifier
      velocypack::Slice json            // view definition
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief drop view in coordinator
  //////////////////////////////////////////////////////////////////////////////
  Result dropViewCoordinator(           // drop view
      std::string const& databaseName,  // database name
      std::string const& viewID         // view identifier
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set view properties in coordinator
  //////////////////////////////////////////////////////////////////////////////

  Result setViewPropertiesCoordinator(std::string const& databaseName,
                                      std::string const& viewID, VPackSlice const& json);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ensure an index in coordinator.
  //////////////////////////////////////////////////////////////////////////////
  Result ensureIndexCoordinator(        // create index
      LogicalCollection const& collection,
      arangodb::velocypack::Slice const& slice, bool create,
      arangodb::velocypack::Builder& resultBuilder,
      double timeout);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief drop an index in coordinator.
  //////////////////////////////////////////////////////////////////////////////
  Result dropIndexCoordinator(          // drop index
      std::string const& databaseName,  // database name
      std::string const& collectionID,  // collection identifier
      TRI_idx_iid_t iid,                // index identifier
      double timeout                    // request timeout
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief (re-)load the information about servers from the agency
  /// Usually one does not have to call this directly.
  //////////////////////////////////////////////////////////////////////////////

  void loadServers();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief find the endpoint of a server from its ID.
  /// If it is not found in the cache, the cache is reloaded once, if
  /// it is still not there an empty string is returned as an error.
  //////////////////////////////////////////////////////////////////////////////

  std::string getServerEndpoint(ServerID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief find the advertised endpoint of a server from its ID.
  /// If it is not found in the cache, the cache is reloaded once, if
  /// it is still not there an empty string is returned as an error.
  //////////////////////////////////////////////////////////////////////////////

  std::string getServerAdvertisedEndpoint(ServerID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief find the server ID for an endpoint.
  /// If it is not found in the cache, the cache is reloaded once, if
  /// it is still not there an empty string is returned as an error.
  //////////////////////////////////////////////////////////////////////////////

  std::string getServerName(std::string const& endpoint);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief (re-)load the information about all coordinators from the agency
  /// Usually one does not have to call this directly.
  //////////////////////////////////////////////////////////////////////////////

  void loadCurrentCoordinators();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief (re-)load the mappings between different IDs/names from the agency
  /// Usually one does not have to call this directly.
  //////////////////////////////////////////////////////////////////////////////

  void loadCurrentMappings();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief (re-)load the information about all DBservers from the agency
  /// Usually one does not have to call this directly.
  //////////////////////////////////////////////////////////////////////////////

  void loadCurrentDBServers();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return a list of all DBServers in the cluster that have
  /// currently registered
  //////////////////////////////////////////////////////////////////////////////

  std::vector<ServerID> getCurrentDBServers();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief find the servers who are responsible for a shard (one leader
  /// and possibly multiple followers).
  /// If it is not found in the cache, the cache is reloaded once, if
  /// it is still not there a pointer to an empty vector is returned as
  /// an error.
  //////////////////////////////////////////////////////////////////////////////

  std::shared_ptr<std::vector<ServerID>> getResponsibleServer(ShardID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief atomically find all servers who are responsible for the given
  /// shards (only the leaders).
  /// will throw an exception if no leader can be found for any
  /// of the shards. will return an empty result if the shards couldn't be
  /// determined after a while - it is the responsibility of the caller to
  /// check for an empty result!
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_map<ShardID, ServerID> getResponsibleServers(std::unordered_set<ShardID> const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief find the shard list of a collection, sorted numerically
  //////////////////////////////////////////////////////////////////////////////

  std::shared_ptr<std::vector<ShardID>> getShardList(CollectionID const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return the list of coordinator server names
  //////////////////////////////////////////////////////////////////////////////

  std::vector<ServerID> getCurrentCoordinators();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief lookup a full coordinator ID by short ID
  //////////////////////////////////////////////////////////////////////////////

  ServerID getCoordinatorByShortID(ServerShortID);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief invalidate planned
  //////////////////////////////////////////////////////////////////////////////

  void invalidatePlan();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief invalidate current
  //////////////////////////////////////////////////////////////////////////////

  void invalidateCurrent();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief invalidate current coordinators
  //////////////////////////////////////////////////////////////////////////////

  void invalidateCurrentCoordinators();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief invalidate current id mappings
  //////////////////////////////////////////////////////////////////////////////

  void invalidateCurrentMappings();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get current "Plan" structure
  //////////////////////////////////////////////////////////////////////////////

  std::shared_ptr<VPackBuilder> getPlan();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get current "Current" structure
  //////////////////////////////////////////////////////////////////////////////

  std::shared_ptr<VPackBuilder> getCurrent();

  std::vector<std::string> getFailedServers() {
    MUTEX_LOCKER(guard, _failedServersMutex);
    return _failedServers;
  }
  void setFailedServers(std::vector<std::string> const& failedServers) {
    MUTEX_LOCKER(guard, _failedServersMutex);
    _failedServers = failedServers;
  }

  std::unordered_map<ServerID, std::string> getServers();

  TEST_VIRTUAL std::unordered_map<ServerID, std::string> getServerAliases();

  std::unordered_map<ServerID, std::string> getServerAdvertisedEndpoints();

  std::unordered_map<ServerID, std::string> getServerTimestamps();

  std::unordered_map<ServerID, RebootId> rebootIds() const;

  uint64_t getPlanVersion() {
    READ_LOCKER(guard, _planProt.lock);
    return _planVersion;
  }

  uint64_t getCurrentVersion() {
    READ_LOCKER(guard, _currentProt.lock);
    return _currentVersion;
  }

  /**
   * @brief Get sorted list of DB server, which serve a shard
   *
   * @param shardId  The id of said shard
   * @return         List of DB servers serving the shard
   */
  arangodb::Result getShardServers(ShardID const& shardId, std::vector<ServerID>&);

  /**
   * @brief Lock agency's hot backup with TTL 60 seconds
   *
   * @param  timeout  Timeout to wait for in seconds
   * @return          Operation's result
   */
  arangodb::Result agencyHotBackupLock(
    std::string const& uuid, double const& timeout, bool& supervisionOff);

  /**
   * @brief Lock agency's hot backup with TTL 60 seconds
   *
   * @param  timeout  Timeout to wait for in seconds
   * @return          Operation's result
   */
  arangodb::Result agencyHotBackupUnlock(
    std::string const& uuid, double const& timeout, const bool& supervisionOff);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get an operation timeout
  //////////////////////////////////////////////////////////////////////////////

  static double getTimeout(double timeout) {
    if (timeout == 0.0) {
      return 24.0 * 3600.0;
    }
    return timeout;
  }

  application_features::ApplicationServer& server() const;

 private:
  void buildIsBuildingSlice(CreateDatabaseInfo const& database,
                              VPackBuilder& builder);

  void buildFinalSlice(CreateDatabaseInfo const& database,
                         VPackBuilder& builder);

  Result waitForDatabaseInCurrent(CreateDatabaseInfo const& database);
  void loadClusterId();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get the poll interval
  //////////////////////////////////////////////////////////////////////////////

  double getPollInterval() const { return 5.0; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get the timeout for reloading the server list
  //////////////////////////////////////////////////////////////////////////////

  double getReloadServerListTimeout() const { return 60.0; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ensure an index in coordinator.
  //////////////////////////////////////////////////////////////////////////////
  Result ensureIndexCoordinatorInner(   // create index
      LogicalCollection const& collection,
      std::string const& idString,
      arangodb::velocypack::Slice const& slice, bool create,
      arangodb::velocypack::Builder& resultBuilder,
      double timeout  // request timeout
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief triggers a new background thread to obtain the next batch of ids
  //////////////////////////////////////////////////////////////////////////////
  void triggerBackgroundGetIds();

  /// underlying application server
  application_features::ApplicationServer& _server;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief object for agency communication
  //////////////////////////////////////////////////////////////////////////////

  AgencyComm _agency;

  AgencyCallbackRegistry* _agencyCallbackRegistry;

  // Cached data from the agency, we reload whenever necessary:

  // We group the data, each group has an atomic "valid-flag" which is
  // used for lazy loading in the beginning. It starts as false, is set
  // to true at each reload and is only reset to false if the cache
  // needs to be invalidated. The variable is atomic to be able to check
  // it without acquiring the read lock (see below). Flush is just an
  // explicit reload for all data and is only used in tests.
  // Furthermore, each group has a mutex that protects against
  // simultaneously contacting the agency for an update.
  // In addition, each group has two atomic version numbers, these are
  // used to prevent a stampede if multiple threads notice concurrently
  // that an update from the agency is necessary. Finally, there is a
  // read/write lock which protects the actual data structure.
  // We encapsulate this protection in the struct ProtectionData:

  struct ProtectionData {
    std::atomic<bool> isValid;
    mutable Mutex mutex;
    std::atomic<uint64_t> wantedVersion;
    std::atomic<uint64_t> doneVersion;
    arangodb::basics::ReadWriteLock lock;

    ProtectionData() : isValid(false), wantedVersion(0), doneVersion(0) {}
  };

  cluster::RebootTracker _rebootTracker;

  // The servers, first all, we only need Current here:
  std::unordered_map<ServerID, std::string> _servers;  // from Current/ServersRegistered
  std::unordered_map<ServerID, std::string> _serverAliases;  // from Current/ServersRegistered
  std::unordered_map<ServerID, std::string> _serverAdvertisedEndpoints;  // from Current/ServersRegistered
  std::unordered_map<ServerID, std::string> _serverTimestamps;      // from Current/ServersRegistered
  ProtectionData _serversProt;

  // Current/ServersKnown:
  ServersKnown _serversKnown;

  // The DBServers, also from Current:
  std::unordered_map<ServerID, ServerID> _DBServers;  // from Current/DBServers
  ProtectionData _DBServersProt;

  // The Coordinators, also from Current:
  std::unordered_map<ServerID, ServerID> _coordinators;  // from Current/Coordinators
  ProtectionData _coordinatorsProt;

  // Mappings between short names/IDs and full server IDs
  std::unordered_map<ServerShortID, ServerID> _coordinatorIdMap;
  ProtectionData _mappingsProt;

  std::shared_ptr<VPackBuilder> _plan;
  std::shared_ptr<VPackBuilder> _current;

  std::string _clusterId;

  std::unordered_map<DatabaseID, VPackSlice> _plannedDatabases;  // from Plan/Databases

  ProtectionData _planProt;

  uint64_t _planVersion;     // This is the version in the Plan which underlies
                             // the data in _plannedCollections, _shards and
                             // _shardKeys
  uint64_t _currentVersion;  // This is the version in Current which underlies
                             // the data in _currentDatabases,
                             // _currentCollections and _shardsIds
  std::unordered_map<DatabaseID, std::unordered_map<ServerID, VPackSlice>> _currentDatabases;  // from Current/Databases
  ProtectionData _currentProt;

  // We need information about collections, again we have
  // data from Plan and from Current.
  // The information for _shards and _shardKeys are filled from the
  // Plan (since they are fixed for the lifetime of the collection).
  // _shardIds is filled from Current, since we have to be able to
  // move shards between servers, and Plan contains who ought to be
  // responsible and Current contains the actual current responsibility.

  // The Plan state:
  AllCollections _plannedCollections;  // from Plan/Collections/
  std::unordered_map<CollectionID,
                     std::shared_ptr<std::vector<std::string>>>
      _shards;  // from Plan/Collections/
                // (may later come from Current/Collections/ )
  std::unordered_map<CollectionID,
                     std::shared_ptr<std::vector<std::string>>>
      _shardKeys;  // from Plan/Collections/
  // planned shard => servers map
  std::unordered_map<ShardID, std::vector<ServerID>> _shardServers;

  AllViews _plannedViews;     // from Plan/Views/
  AllViews _newPlannedViews;  // views that have been created during `loadPlan`
                              // execution
  std::atomic<std::thread::id> _planLoader;  // thread id that is loading plan

  // The Current state:
  AllCollectionsCurrent _currentCollections;  // from Current/Collections/
  std::unordered_map<ShardID, std::shared_ptr<std::vector<ServerID>>> _shardIds;  // from Current/Collections/

  //////////////////////////////////////////////////////////////////////////////
  /// @brief uniqid sequence
  //////////////////////////////////////////////////////////////////////////////

  struct {
    uint64_t _currentValue;
    uint64_t _upperValue;
    uint64_t _nextBatchStart;
    uint64_t _nextUpperValue;
    bool _backgroundJobIsRunning;
  } _uniqid;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief lock for uniqid sequence
  //////////////////////////////////////////////////////////////////////////////

  Mutex _idLock;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the sole instance
  //////////////////////////////////////////////////////////////////////////////

  static ClusterInfo* _theinstance;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief how big a batch is for unique ids
  //////////////////////////////////////////////////////////////////////////////

  static uint64_t const MinIdsPerBatch = 1000000;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief default wait timeout
  //////////////////////////////////////////////////////////////////////////////

  static double const operationTimeout;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief reload timeout
  //////////////////////////////////////////////////////////////////////////////

  static double const reloadServerListTimeout;

  arangodb::Mutex _failedServersMutex;
  std::vector<std::string> _failedServers;
};

}  // end namespace arangodb

#endif
