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
/// @author Kaveh Vahedipour
/// @author Matthew Von-Maszewski
////////////////////////////////////////////////////////////////////////////////

#include "SynchronizeShard.h"

#include "Agency/AgencyStrings.h"
#include "Agency/TimeString.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ActionDescription.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/MaintenanceFeature.h"
#include "Cluster/ServerState.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "Replication/DatabaseInitialSyncer.h"
#include "Replication/DatabaseReplicationApplier.h"
#include "Replication/DatabaseTailingSyncer.h"
#include "Replication/GlobalInitialSyncer.h"
#include "Replication/GlobalReplicationApplier.h"
#include "Replication/ReplicationApplierConfiguration.h"
#include "Replication/ReplicationFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/ServerIdFeature.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Databases.h"

#include <velocypack/Compare.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb::application_features;
using namespace arangodb::maintenance;
using namespace arangodb::methods;
using namespace arangodb::transaction;
using namespace arangodb;
using namespace arangodb::consensus;

std::string const ENDPOINT("endpoint");
std::string const INCLUDE("include");
std::string const INCLUDE_SYSTEM("includeSystem");
std::string const INCREMENTAL("incremental");
std::string const KEEP_BARRIER("keepBarrier");
std::string const LEADER_ID("leaderId");
std::string const BARRIER_ID("barrierId");
std::string const LAST_LOG_TICK("lastLogTick");
std::string const API_REPLICATION("/_api/replication/");
std::string const REPL_ADD_FOLLOWER(API_REPLICATION + "addFollower");
std::string const REPL_BARRIER_API(API_REPLICATION + "barrier/");
std::string const REPL_HOLD_READ_LOCK(API_REPLICATION +
                                      "holdReadLockCollection");
std::string const REPL_REM_FOLLOWER(API_REPLICATION + "removeFollower");
std::string const RESTRICT_TYPE("restrictType");
std::string const RESTRICT_COLLECTIONS("restrictCollections");
std::string const SKIP_CREATE_DROP("skipCreateDrop");
std::string const TTL("ttl");

using namespace std::chrono;

SynchronizeShard::SynchronizeShard(MaintenanceFeature& feature, ActionDescription const& desc)
    : ActionBase(feature, desc) {
  std::stringstream error;

  if (!desc.has(COLLECTION)) {
    error << "collection must be specified";
  }
  TRI_ASSERT(desc.has(COLLECTION));

  if (!desc.has(DATABASE)) {
    error << "database must be specified";
  }
  TRI_ASSERT(desc.has(DATABASE));

  if (!desc.has(SHARD)) {
    error << "SHARD must be specified";
  }
  TRI_ASSERT(desc.has(SHARD));

  if (!desc.has(THE_LEADER) || desc.get(THE_LEADER).empty()) {
    error << "leader must be specified and must be non-empty";
  }
  TRI_ASSERT(desc.has(THE_LEADER) && !desc.get(THE_LEADER).empty());

  if (!desc.has(SHARD_VERSION)) {
    error << "local shard version must be specified. ";
  }
  TRI_ASSERT(desc.has(SHARD_VERSION));

  if (!error.str().empty()) {
    LOG_TOPIC("03780", ERR, Logger::MAINTENANCE) << "SynchronizeShard: " << error.str();
    _result.reset(TRI_ERROR_INTERNAL, error.str());
    setState(FAILED);
  }
}

SynchronizeShard::~SynchronizeShard() = default;

static std::stringstream& AppendShardInformationToMessage(
    std::string const& database, std::string const& shard, std::string const& planId,
    std::chrono::system_clock::time_point const& startTime, std::stringstream& msg) {
  auto const endTime = system_clock::now();
  msg << "local shard: '" << database << "/" << shard << "', "
      << "for central: '" << database << "/" << planId << "', "
      << "started: " << timepointToString(startTime) << ", "
      << "ended: " << timepointToString(endTime);
  return msg;
}

static arangodb::Result getReadLockId(network::ConnectionPool* pool,
                                      std::string const& endpoint,
                                      std::string const& database, std::string const& clientId,
                                      double timeout, uint64_t& id) {
  std::string error("startReadLockOnLeader: Failed to get read lock - ");

  if (pool == nullptr) {  // nullptr only happens during controlled shutdown
    return arangodb::Result(TRI_ERROR_SHUTTING_DOWN,
                            "startReadLockOnLeader: Shutting down");
  }

  network::RequestOptions options;
  options.database = database;
  options.timeout = network::Timeout(timeout);
  options.skipScheduler = true; // hack to speed up future.get()
  
  auto res = network::sendRequest(pool, endpoint, fuerte::RestVerb::Get,
                                  REPL_HOLD_READ_LOCK,
                                  VPackBuffer<uint8_t>(), options)
                 .get();

  if (res.ok() && res.response->statusCode() == fuerte::StatusOK) {
    auto const idSlice = res.response->slice();
    TRI_ASSERT(idSlice.isObject());
    TRI_ASSERT(idSlice.hasKey(ID));
    try {
      id = std::stoull(idSlice.get(ID).copyString());
    } catch (std::exception const&) {
      error += " expecting id to be uint64_t ";
      error += idSlice.toJson();
      return arangodb::Result(TRI_ERROR_INTERNAL, error);
    }
  } else {
    error.append(network::fuerteToArangoErrorMessage(res));
    return arangodb::Result(TRI_ERROR_INTERNAL, error);
  }

  return arangodb::Result();
}

static arangodb::Result collectionCount(std::shared_ptr<arangodb::LogicalCollection> const& col,
                                        uint64_t& c) {
  std::string collectionName(col->name());
  auto ctx = std::make_shared<transaction::StandaloneContext>(col->vocbase());
  SingleCollectionTransaction trx(ctx, collectionName, AccessMode::Type::READ);

  Result res = trx.begin();
  if (res.fail()) {
    LOG_TOPIC("5be16", ERR, Logger::MAINTENANCE) << "Failed to start count transaction: " << res;
    return res;
  }

  OperationResult opResult =
      trx.count(collectionName, arangodb::transaction::CountType::Normal);
  res = trx.finish(opResult.result);

  if (res.fail()) {
    LOG_TOPIC("263d2", ERR, Logger::MAINTENANCE)
        << "Failed to finish count transaction: " << res;
    return res;
  }

  VPackSlice s = opResult.slice();
  TRI_ASSERT(s.isNumber());
  c = s.getNumber<uint64_t>();

  return opResult.result;
}

static arangodb::Result addShardFollower(
    network::ConnectionPool* pool, std::string const& endpoint,
    std::string const& database, std::string const& shard, uint64_t lockJobId,
    std::string const& clientId, SyncerId const syncerId,
    std::string const& clientInfoString, double timeout = 120.0) {
  LOG_TOPIC("b982e", DEBUG, Logger::MAINTENANCE)
      << "addShardFollower: tell the leader to put us into the follower "
         "list...";

  if (pool == nullptr) {  // nullptr only happens during controlled shutdown
    return arangodb::Result(TRI_ERROR_SHUTTING_DOWN,
                            "startReadLockOnLeader: Shutting down");
  }

  try {
    DatabaseGuard guard(database);
    auto vocbase = &guard.database();

    auto collection = vocbase->lookupCollection(shard);
    if (collection == nullptr) {
      std::string errorMsg(
          "SynchronizeShard::addShardFollower: Failed to lookup collection ");
      errorMsg += shard;
      LOG_TOPIC("4a8db", ERR, Logger::MAINTENANCE) << errorMsg;
      return arangodb::Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, errorMsg);
    }

    uint64_t docCount;
    {
      Result res = collectionCount(collection, docCount);
      if (res.fail()) {
        return res;
      }
    }
    VPackBuilder body;
    {
      VPackObjectBuilder b(&body);
      body.add(FOLLOWER_ID, VPackValue(arangodb::ServerState::instance()->getId()));
      body.add(SHARD, VPackValue(shard));
      body.add("checksum", VPackValue(std::to_string(docCount)));
      body.add("serverId", VPackValue(basics::StringUtils::itoa(ServerIdFeature::getId())));
      if (syncerId.value != 0) {
        body.add("syncerId", VPackValue(syncerId.toString()));
      }
      if (!clientInfoString.empty()) {
        body.add("clientInfo", VPackValue(clientInfoString));
      }
      if (lockJobId != 0) {
        body.add("readLockId", VPackValue(std::to_string(lockJobId)));
      } else {  // short cut case
        if (docCount != 0) {
          // This can happen if we once were an in-sync follower and a
          // synchronization request has timed out, but still runs on our
          // side here. In this case, we can simply continue with the slow
          // path and run the full sync protocol. Therefore we error out
          // here. Note that we are in the lockJobId == 0 case, which is
          // the shortcut.
          std::string msg =
              "Short cut synchronization for shard " + shard +
              " did not work, since we got a document in the meantime.";
          LOG_TOPIC("ef299", INFO, Logger::MAINTENANCE) << msg;
          return arangodb::Result(TRI_ERROR_INTERNAL, msg);
        }
      }
    }
    
    network::RequestOptions options;
    options.database = database;
    options.timeout = network::Timeout(timeout);
    options.skipScheduler = true; // hack to speed up future.get()
    
    auto res = network::sendRequest(pool, endpoint, fuerte::RestVerb::Put,
                                    REPL_ADD_FOLLOWER,
                                    std::move(*body.steal()), options)
                   .get();

    std::string errorMessage(
        "addShardFollower: could not add us to the leader's follower list. ");
    if (res.fail() || res.response->statusCode() != fuerte::StatusOK) {
      if (lockJobId != 0) {
        errorMessage += network::fuerteToArangoErrorMessage(res);
        LOG_TOPIC("22e0a", ERR, Logger::MAINTENANCE) << errorMessage;
      } else {
        errorMessage += "With shortcut (can happen, no problem).";
        LOG_TOPIC("abf2e", INFO, Logger::MAINTENANCE) << errorMessage;
      }
      return arangodb::Result(TRI_ERROR_INTERNAL, errorMessage);
    }

    LOG_TOPIC("79935", DEBUG, Logger::MAINTENANCE) << "addShardFollower: success";
    return arangodb::Result();
  } catch (std::exception const& e) {
    std::string errorMsg(
        "SynchronizeShard::addShardFollower: Failed to lookup database ");
    errorMsg += database;
    errorMsg += " exception: ";
    errorMsg += e.what();
    LOG_TOPIC("6b7e8", ERR, Logger::MAINTENANCE) << errorMsg;
    return arangodb::Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND, errorMsg);
  }
}

static arangodb::Result cancelReadLockOnLeader(network::ConnectionPool* pool,
                                               std::string const& endpoint,
                                               std::string const& database, uint64_t lockJobId,
                                               std::string const& clientId,
                                               double timeout = 10.0) {
  if (pool == nullptr) {  // nullptr only happens during controlled shutdown
    return arangodb::Result(TRI_ERROR_SHUTTING_DOWN,
                            "cancelReadLockOnLeader: Shutting down");
  }

  VPackBuilder body;
  {
    VPackObjectBuilder b(&body);
    body.add(ID, VPackValue(std::to_string(lockJobId)));
  }

  network::RequestOptions options;
  options.database = database;
  options.timeout = network::Timeout(timeout);
  options.skipScheduler = true; // hack to speed up future.get()
  
  auto res = network::sendRequest(pool, endpoint, fuerte::RestVerb::Delete,
                                  REPL_HOLD_READ_LOCK,
                                  std::move(*body.steal()), options)
                 .get();

  if (res.ok() && res.response && res.response->statusCode() == fuerte::StatusNotFound) {
    auto const slice = res.response->slice();
    if (slice.isObject()) {
      VPackSlice s = slice.get(StaticStrings::ErrorNum);
      if (s.isNumber()) {
        int errorNum = s.getNumber<int>();
        if (errorNum == TRI_ERROR_ARANGO_DATABASE_NOT_FOUND) {
          // database is gone. that means our lock is also gone
          return arangodb::Result();
        }
      }
    }
  }

  if (res.fail() || res.response->statusCode() != fuerte::StatusOK) {
    auto errorMessage = network::fuerteToArangoErrorMessage(res);
    // rebuild body since we stole it earlier
    VPackBuilder body;
    {
      VPackObjectBuilder b(&body);
      body.add(ID, VPackValue(std::to_string(lockJobId)));
    }
    LOG_TOPIC("52924", ERR, Logger::MAINTENANCE)
        << "cancelReadLockOnLeader: exception caught for " << body.toJson()
        << ": " << errorMessage;
    return arangodb::Result(TRI_ERROR_INTERNAL, errorMessage);
  }

  LOG_TOPIC("4355c", DEBUG, Logger::MAINTENANCE) << "cancelReadLockOnLeader: success";
  return arangodb::Result();
}

static arangodb::Result cancelBarrier(network::ConnectionPool* pool,
                                      std::string const& endpoint,
                                      std::string const& database, int64_t barrierId,
                                      std::string const& clientId, double timeout = 120.0) {
  if (barrierId <= 0) {
    return Result();
  }

  if (pool == nullptr) {  // nullptr only happens during controlled shutdown
    return arangodb::Result(TRI_ERROR_SHUTTING_DOWN,
                            "startReadLockOnLeader: Shutting down");
  }

  network::RequestOptions options;
  options.database = database;
  options.timeout = network::Timeout(timeout);
  options.skipScheduler = true; // hack to speed up future.get()
  
  auto res =
      network::sendRequest(pool, endpoint, fuerte::RestVerb::Delete,
                           REPL_BARRIER_API + std::to_string(barrierId),
                           VPackBuffer<uint8_t>(), options)
          .get();

  if (res.ok()) {
    auto* response = res.response.get();
    if (response->statusCode() != fuerte::StatusOK &&
        response->statusCode() != fuerte::StatusNoContent) {
      std::string errorMessage = "got status " + std::to_string(response->statusCode());
      LOG_TOPIC("f5733", ERR, Logger::MAINTENANCE)
          << "CancelBarrier: error '" << errorMessage << "'";
      return arangodb::Result(TRI_ERROR_INTERNAL, errorMessage);
    }
  } else {
    std::string error(
        "CancelBarrier: failed to send message to leader : status ");
    error += network::fuerteToArangoErrorMessage(res);
    LOG_TOPIC("1c48a", ERR, Logger::MAINTENANCE) << error;
    return arangodb::Result(TRI_ERROR_INTERNAL, error);
  }

  LOG_TOPIC("313dc", DEBUG, Logger::MAINTENANCE) << "cancelBarrier: success";
  return arangodb::Result();
}

static inline bool isStopping() {
  auto& server = application_features::ApplicationServer::server();
  return server.isStopping();
}

arangodb::Result SynchronizeShard::getReadLock(network::ConnectionPool* pool,
                                               std::string const& endpoint,
                                               std::string const& database,
                                               std::string const& collection,
                                               std::string const& clientId, uint64_t rlid,
                                               bool soft, double timeout) {
  if (pool == nullptr) {  // nullptr only happens during controlled shutdown
    return arangodb::Result(TRI_ERROR_SHUTTING_DOWN,
                            "startReadLockOnLeader: Shutting down");
  }

  VPackBuilder bodyBuilder;
  {
    VPackObjectBuilder o(&bodyBuilder);
    bodyBuilder.add(ID, VPackValue(std::to_string(rlid)));
    bodyBuilder.add(COLLECTION, VPackValue(collection));
    bodyBuilder.add(TTL, VPackValue(timeout));
    bodyBuilder.add(StaticStrings::ReplicationSoftLockOnly, VPackValue(soft));
  }
  auto body = bodyBuilder.steal();

  network::RequestOptions options;
  options.database = database;
  options.timeout = network::Timeout(timeout);
  options.skipScheduler = true; // hack to speed up future.get()
  
  auto dummy = network::sendRequest(pool, endpoint, fuerte::RestVerb::Post, REPL_HOLD_READ_LOCK,
                                    *body, options);

  // Intentionally do not look at the outcome, even in case of an error
  // we must make sure that the read lock on the leader is not active!
  // This is done automatically below.
  
  size_t const maxTries = 9; // 511s max
  double sleepTime = 1.0;
  size_t count = 0;
  while (++count < maxTries) {  // wait for some time until read lock established:

    if (isStopping()) {
      return arangodb::Result(TRI_ERROR_SHUTTING_DOWN);
    }

    // Now check that we hold the read lock:
    auto res = network::sendRequest(pool, endpoint, fuerte::RestVerb::Put, REPL_HOLD_READ_LOCK,
                                    *body, options)
                   .get();

    if (res.ok() && res.response->statusCode() == fuerte::StatusOK) {
      auto const slice = res.response->slice();
      TRI_ASSERT(slice.isObject());
      VPackSlice lockHeld = slice.get("lockHeld");
      if (lockHeld.isBoolean() && lockHeld.getBool()) {
        return arangodb::Result();
      }
      LOG_TOPIC("b681f", DEBUG, Logger::MAINTENANCE)
          << "startReadLockOnLeader: Lock not yet acquired...";
    } else {
      if (res.ok() && res.response->statusCode() == fuerte::StatusNotFound) {
        auto const slice = res.response->slice();
        if (slice.isObject()) {
          VPackSlice s = slice.get(StaticStrings::ErrorNum);
          if (s.isNumber()) {
            int errorNum = s.getNumber<int>();
            if (errorNum == TRI_ERROR_ARANGO_DATABASE_NOT_FOUND) {
              // database is gone. we can now give up
              break;
            }
          }
        }
        // fall-through to other cases intentional here
      }

      std::string message = network::fuerteToArangoErrorMessage(res);
      LOG_TOPIC("a82bc", DEBUG, Logger::MAINTENANCE)
          << "startReadLockOnLeader: Do not see read lock yet:" << message;
    }

    std::this_thread::sleep_for(duration<double>(sleepTime));
    sleepTime *= 2.0;
  }

  LOG_TOPIC("75e2b", ERR, Logger::MAINTENANCE) << "startReadLockOnLeader: giving up";

  try {
    auto r = network::sendRequest(pool, endpoint, fuerte::RestVerb::Delete, REPL_HOLD_READ_LOCK,
                                  *body, options)
                 .get();
    if (r.fail() || r.response->statusCode() != fuerte::StatusOK) {
      std::string addendum = network::fuerteToArangoErrorMessage(r);
      LOG_TOPIC("4f34d", ERR, Logger::MAINTENANCE)
          << "startReadLockOnLeader: cancelation error for shard - "
          << collection << ": " << addendum;
    }
  } catch (std::exception const& e) {
    LOG_TOPIC("7fcc9", ERR, Logger::MAINTENANCE)
        << "startReadLockOnLeader: exception in cancel: " << e.what();
  }

  return arangodb::Result(TRI_ERROR_CLUSTER_TIMEOUT,
                          "startReadLockOnLeader: giving up");
}

arangodb::Result SynchronizeShard::startReadLockOnLeader(
    std::string const& endpoint, std::string const& database, std::string const& collection,
    std::string const& clientId, uint64_t& rlid, bool soft, double timeout) {
  // Read lock id
  rlid = 0;
  NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  arangodb::Result result =
      getReadLockId(pool, endpoint, database, clientId, timeout, rlid);
  if (!result.ok()) {
    LOG_TOPIC("2e5ae", ERR, Logger::MAINTENANCE) << result.errorMessage();
    return result;
  }
  LOG_TOPIC("c8d18", DEBUG, Logger::MAINTENANCE) << "Got read lock id: " << rlid;

  result = getReadLock(pool, endpoint, database, collection, clientId, rlid, soft, timeout);

  return result;
}

enum ApplierType { APPLIER_DATABASE, APPLIER_GLOBAL };

static arangodb::ResultT<SyncerId> replicationSynchronize(
    std::shared_ptr<arangodb::LogicalCollection> const& col,
    VPackSlice const& config, std::string const& clientInfoString,
    ApplierType applierType, std::shared_ptr<VPackBuilder> sy) {
  auto& vocbase = col->vocbase();

  auto database = vocbase.name();

  auto shard = col->name();

  bool keepBarrier = config.get(KEEP_BARRIER).getBool();
  std::string leaderId;
  if (config.hasKey(LEADER_ID)) {
    leaderId = config.get(LEADER_ID).copyString();
  }

  ReplicationApplierConfiguration configuration =
      ReplicationApplierConfiguration::fromVelocyPack(vocbase.server(), config, database);
  configuration.setClientInfo(clientInfoString);
  configuration.validate();

  std::shared_ptr<InitialSyncer> syncer;

  if (applierType == APPLIER_DATABASE) {
    // database-specific synchronization
    syncer.reset(new DatabaseInitialSyncer(vocbase, configuration));

    if (!leaderId.empty()) {
      syncer->setLeaderId(leaderId);
    }
  } else if (applierType == APPLIER_GLOBAL) {
    configuration._skipCreateDrop = false;
    syncer.reset(new GlobalInitialSyncer(configuration));
  } else {
    TRI_ASSERT(false);
  }

  SyncerId syncerId{syncer->syncerId()};

  try {
    Result r = syncer->run(configuration._incremental);

    if (r.fail()) {
      LOG_TOPIC("3efff", DEBUG, Logger::REPLICATION)
          << "initial sync failed for database '" << database
          << "': " << r.errorMessage();
      THROW_ARANGO_EXCEPTION_MESSAGE(r.errorNumber(), r.errorMessage());
    }

    {
      VPackObjectBuilder o(sy.get());
      if (keepBarrier) {
        sy->add(BARRIER_ID, VPackValue(syncer->stealBarrier()));
      }
      sy->add(LAST_LOG_TICK, VPackValue(syncer->getLastLogTick()));
      sy->add(VPackValue(COLLECTIONS));
      {
        VPackArrayBuilder a(sy.get());
        for (auto const& i : syncer->getProcessedCollections()) {
          VPackObjectBuilder e(sy.get());
          sy->add(ID, VPackValue(i.first));
          sy->add(NAME, VPackValue(i.second));
        }
      }
    }

  } catch (arangodb::basics::Exception const& ex) {
    std::string s("cannot sync from remote endpoint: ");
    s += ex.what() + std::string(". last progress message was '") +
         syncer->progress() + "'";
    return Result(ex.code(), s);
  } catch (std::exception const& ex) {
    std::string s("cannot sync from remote endpoint: ");
    s += ex.what() + std::string(". last progress message was '") +
         syncer->progress() + "'";
    return Result(TRI_ERROR_INTERNAL, s);
  } catch (...) {
    std::string s(
        "cannot sync from remote endpoint: unknown exception. last progress "
        "message was '");
    s += syncer->progress() + "'";
    return Result(TRI_ERROR_INTERNAL, s);
  }

  return ResultT<SyncerId>::success(syncerId);
}

static arangodb::Result replicationSynchronizeCatchup(VPackSlice const& conf, double timeout,
                                                      TRI_voc_tick_t& tickReached,
                                                      bool& didTimeout) {
  didTimeout = false;

  auto const database = conf.get(DATABASE).copyString();
  auto const collection = conf.get(COLLECTION).copyString();
  auto const leaderId = conf.get(LEADER_ID).copyString();
  auto const fromTick = conf.get("from").getNumber<uint64_t>();

  auto& server = application_features::ApplicationServer::server();
  ReplicationApplierConfiguration configuration =
      ReplicationApplierConfiguration::fromVelocyPack(server, conf, database);
  // will throw if invalid
  configuration.validate();

  DatabaseGuard guard(database);
  DatabaseTailingSyncer syncer(guard.database(), configuration, fromTick, true, 0);

  if (!leaderId.empty()) {
    syncer.setLeaderId(leaderId);
  }

  Result r;
  try {
    r = syncer.syncCollectionCatchup(collection, timeout, tickReached, didTimeout);
  } catch (arangodb::basics::Exception const& ex) {
    r = Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    r = Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    r = Result(TRI_ERROR_INTERNAL, "unknown exception");
  }

  if (r.fail()) {
    LOG_TOPIC("fa2ab", ERR, Logger::REPLICATION)
        << "syncCollectionFinalize failed: " << r.errorMessage();
  }

  return r;
}

static arangodb::Result replicationSynchronizeFinalize(VPackSlice const& conf) {
  auto const database = conf.get(DATABASE).copyString();
  auto const collection = conf.get(COLLECTION).copyString();
  auto const leaderId = conf.get(LEADER_ID).copyString();
  auto const fromTick = conf.get("from").getNumber<uint64_t>();

  auto& server = application_features::ApplicationServer::server();
  ReplicationApplierConfiguration configuration =
      ReplicationApplierConfiguration::fromVelocyPack(server, conf, database);
  // will throw if invalid
  configuration.validate();

  DatabaseGuard guard(database);
  DatabaseTailingSyncer syncer(guard.database(), configuration, fromTick, true, 0);

  if (!leaderId.empty()) {
    syncer.setLeaderId(leaderId);
  }

  Result r;
  try {
    r = syncer.syncCollectionFinalize(collection);
  } catch (arangodb::basics::Exception const& ex) {
    r = Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    r = Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    r = Result(TRI_ERROR_INTERNAL, "unknown exception");
  }

  if (r.fail()) {
    LOG_TOPIC("e8056", ERR, Logger::REPLICATION)
        << "syncCollectionFinalize failed: " << r.errorMessage();
  }

  return r;
}

bool SynchronizeShard::first() {
  std::string database = _description.get(DATABASE);
  std::string planId = _description.get(COLLECTION);
  std::string shard = _description.get(SHARD);
  std::string leader = _description.get(THE_LEADER);

  LOG_TOPIC("fa651", DEBUG, Logger::MAINTENANCE)
      << "SynchronizeShard: synchronizing shard '" << database << "/" << shard
      << "' for central '" << database << "/" << planId << "'";

  auto& clusterInfo = feature().server().getFeature<ClusterFeature>().clusterInfo();
  auto const ourselves = arangodb::ServerState::instance()->getId();
  auto startTime = system_clock::now();
  auto const startTimeStr = timepointToString(startTime);
  std::string const clientId(database + planId + shard + leader);

  // First wait until the leader has created the shard (visible in
  // Current in the Agency) or we or the shard have vanished from
  // the plan:
  while (true) {
    if (isStopping()) {
      _result.reset(TRI_ERROR_SHUTTING_DOWN);
      return false;
    }

    std::vector<std::string> planned;
    auto result = clusterInfo.getShardServers(shard, planned);

    if (!result.ok() ||
        std::find(planned.begin(), planned.end(), ourselves) == planned.end() ||
        planned.front() != leader) {
      // Things have changed again, simply terminate:
      std::stringstream error;
      error << "cancelled, ";
      AppendShardInformationToMessage(database, shard, planId, startTime, error);
      LOG_TOPIC("a1dc7", DEBUG, Logger::MAINTENANCE) << "SynchronizeOneShard: " << error.str();
      _result.reset(TRI_ERROR_FAILED, error.str());
      return false;
    }

    auto ci = clusterInfo.getCollectionNT(database, planId);
    if (ci == nullptr) {
      std::stringstream msg;
      msg << "exception in getCollection, ";
      AppendShardInformationToMessage(database, shard, planId, startTime, msg);
      LOG_TOPIC("89972", DEBUG, Logger::MAINTENANCE) << "SynchronizeOneShard: " << msg.str();
      _result.reset(TRI_ERROR_FAILED, msg.str());
      return false;
    }

    std::string const cid = std::to_string(ci->id());
    std::shared_ptr<CollectionInfoCurrent> cic =
        clusterInfo.getCollectionCurrent(database, cid);
    std::vector<std::string> current = cic->servers(shard);

    if (current.empty()) {
      LOG_TOPIC("b0ccf", DEBUG, Logger::MAINTENANCE)
          << "synchronizeOneShard: cancelled, no servers in 'Current'";
    } else if (current.front() == leader) {
      if (std::find(current.begin(), current.end(), ourselves) == current.end()) {
        break;  // start synchronization work
      }
      // We are already there, this is rather strange, but never mind:
      std::stringstream error;
      error << "already done, ";
      AppendShardInformationToMessage(database, shard, planId, startTime, error);
      LOG_TOPIC("4abcb", DEBUG, Logger::MAINTENANCE) << "SynchronizeOneShard: " << error.str();
      _result.reset(TRI_ERROR_FAILED, error.str());
      return false;
    }

    LOG_TOPIC("28600", DEBUG, Logger::MAINTENANCE)
        << "synchronizeOneShard: waiting for leader, " << database << "/"
        << shard << ", " << database << "/" << planId;

    std::this_thread::sleep_for(duration<double>(0.2));
  }

  // Once we get here, we know that the leader is ready for sync, so we give it
  // a try:

  try {
    DatabaseGuard guard(database);
    auto vocbase = &guard.database();

    auto collection = vocbase->lookupCollection(shard);
    if (collection == nullptr) {
      std::stringstream error;
      error << "failed to lookup local shard " << shard;
      LOG_TOPIC("06489", ERR, Logger::MAINTENANCE) << "SynchronizeOneShard: " << error.str();
      _result.reset(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, error.str());
      return false;
    }

    auto ep = clusterInfo.getServerEndpoint(leader);
    uint64_t docCount;
    if (!collectionCount(collection, docCount).ok()) {
      std::stringstream error;
      error << "failed to get a count on leader " << shard;
      LOG_TOPIC("da225", ERR, Logger::MAINTENANCE) << "SynchronizeShard " << error.str();
      _result.reset(TRI_ERROR_INTERNAL, error.str());
      return false;
    }

    { // Initialize _clientInfoString
      CollectionNameResolver resolver(collection->vocbase());
      _clientInfoString =
          std::string{"follower "} + ServerState::instance()->getPersistedId() +
          " of shard " + collection->name() + " of collection " + database +
          "/" + resolver.getCollectionName(collection->id());
    }

    if (docCount == 0) {
      // We have a short cut:
      LOG_TOPIC("0932a", DEBUG, Logger::MAINTENANCE)
          << "synchronizeOneShard: trying short cut to synchronize local shard "
             "'"
          << database << "/" << shard << "' for central '" << database << "/"
          << planId << "'";
      try {
        NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
        network::ConnectionPool* pool = nf.pool();
        auto asResult = addShardFollower(pool, ep, database, shard, 0, clientId,
                                         SyncerId{}, _clientInfoString, 60.0);

        if (asResult.ok()) {
          if (Logger::isEnabled(LogLevel::DEBUG, Logger::MAINTENANCE)) {
            std::stringstream msg;
            msg << "synchronizeOneShard: shortcut worked, done, ";
            AppendShardInformationToMessage(database, shard, planId, startTime, msg);
            LOG_TOPIC("f4a5b", DEBUG, Logger::MAINTENANCE) << msg.str();
          }
          collection->followers()->setTheLeader(leader);
          notify();
          return false;
        }
      } catch (...) {
      }
    }

    LOG_TOPIC("53337", DEBUG, Logger::MAINTENANCE)
        << "synchronizeOneShard: trying to synchronize local shard '" << database
        << "/" << shard << "' for central '" << database << "/" << planId << "'";

    try {
      // From here on we perform a number of steps, each of which can
      // fail. If it fails with an exception, it is caught, but this
      // should usually not happen. If it fails without an exception,
      // we log and use return.

      Result res;  // used multiple times for intermediate results

      // First once without a read transaction:

      if (isStopping()) {
        std::string errorMessage(
            "synchronizeOneShard: synchronization failed for shard ");
        errorMessage += shard + ": shutdown in progress, giving up";
        LOG_TOPIC("a0f9a", INFO, Logger::MAINTENANCE) << errorMessage;
        _result.reset(TRI_ERROR_SHUTTING_DOWN, errorMessage);
        return false;
      }

      // This is necessary to accept replications from the leader which can
      // happen as soon as we are in sync.
      collection->followers()->setTheLeader(leader);

      startTime = system_clock::now();

      VPackBuilder config;
      {
        VPackObjectBuilder o(&config);
        config.add(ENDPOINT, VPackValue(ep));
        config.add(INCREMENTAL,
                   VPackValue(docCount > 0));  // use dump if possible
        config.add(KEEP_BARRIER, VPackValue(true));
        config.add(LEADER_ID, VPackValue(leader));
        config.add(SKIP_CREATE_DROP, VPackValue(true));
        config.add(RESTRICT_TYPE, VPackValue(INCLUDE));
        config.add(VPackValue(RESTRICT_COLLECTIONS));
        {
          VPackArrayBuilder a(&config);
          config.add(VPackValue(shard));
        }
        config.add(INCLUDE_SYSTEM, VPackValue(true));
        config.add("verbose", VPackValue(false));
      }

      auto details = std::make_shared<VPackBuilder>();

      ResultT<SyncerId> syncRes =
          replicationSynchronize(collection, config.slice(), _clientInfoString,
                                 APPLIER_DATABASE, details);

      auto sy = details->slice();
      auto const endTime = system_clock::now();

      // Long shard sync initialization
      if (endTime - startTime > seconds(5)) {
        LOG_TOPIC("ca7e3", INFO, Logger::MAINTENANCE)
            << "synchronizeOneShard: long call to syncCollection for shard"
            << shard << " " << syncRes.errorMessage()
            << " start time: " << timepointToString(startTime)
            << ", end time: " << timepointToString(system_clock::now());
      }

      // If this did not work, then we cannot go on:
      if (!syncRes.ok()) {
        std::stringstream error;
        error << "could not initially synchronize shard " << shard << ": "
              << syncRes.errorMessage();
        LOG_TOPIC("c1b31", DEBUG, Logger::MAINTENANCE) << "SynchronizeOneShard: " << error.str();
        _result.reset(TRI_ERROR_INTERNAL, error.str());
        return false;
      }

      SyncerId syncerId = syncRes.get();
      // From here on, we have to call `cancelBarrier` in case of errors
      // as well as in the success case!
      auto barrierId = sy.get(BARRIER_ID).getNumber<int64_t>();
      NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
      network::ConnectionPool* pool = nf.pool();
      TRI_DEFER(cancelBarrier(pool, ep, database, barrierId, clientId));

      VPackSlice collections = sy.get(COLLECTIONS);

      if (collections.length() == 0 || collections[0].get("name").copyString() != shard) {
        std::stringstream error;
        error
            << "shard " << shard
            << " seems to be gone from leader, this "
               "can happen if a collection was dropped during synchronization!";
        LOG_TOPIC("664ae", WARN, Logger::MAINTENANCE) << "SynchronizeOneShard: " << error.str();
        _result.reset(TRI_ERROR_INTERNAL, error.str());
        return false;
      }

      auto lastTick =
          arangodb::basics::VelocyPackHelper::getNumericValue<TRI_voc_tick_t>(sy, LAST_LOG_TICK,
                                                                               0);
      VPackBuilder builder;

      ResultT<TRI_voc_tick_t> tickResult =
          catchupWithReadLock(ep, database, *collection, clientId, shard,
                              leader, lastTick, builder);
      if (!tickResult.ok()) {
        LOG_TOPIC("0a4d4", INFO, Logger::MAINTENANCE) << syncRes.errorMessage();
        _result.reset(tickResult.result());
        return false;
      }
      lastTick = tickResult.get();

      // Now start a exclusive transaction to stop writes:
      res = catchupWithExclusiveLock(ep, database, *collection, clientId, shard,
                                     leader, syncerId, lastTick, builder);
      if (!res.ok()) {
        LOG_TOPIC("be85f", INFO, Logger::MAINTENANCE) << res.errorMessage();
        _result.reset(res);
        return false;
      }

    } catch (std::exception const& e) {
      std::stringstream error;
      error << "synchronization of ";
      AppendShardInformationToMessage(database, shard, planId, startTime, error);
      error << " failed: " << e.what();
      LOG_TOPIC("1e576", ERR, Logger::MAINTENANCE) << error.str();
      _result.reset(TRI_ERROR_INTERNAL, e.what());
      return false;
    }
    // Validate that HARDLOCK only works!
  } catch (std::exception const& e) {
    // This catches the case that we could not even find the collection
    // locally, because the DatabaseGuard constructor threw.
    LOG_TOPIC("9f2c0", WARN, Logger::MAINTENANCE)
        << "action " << _description << " failed with exception " << e.what();
    _result.reset(TRI_ERROR_INTERNAL, e.what());
    return false;
  }

  // Tell others that we are done:
  if (Logger::isEnabled(LogLevel::INFO, Logger::MAINTENANCE)) {
    // This wrap is just to not write the stream if not needed.
    std::stringstream msg;
    AppendShardInformationToMessage(database, shard, planId, startTime, msg);
    LOG_TOPIC("e6780", INFO, Logger::MAINTENANCE) << "synchronizeOneShard: done, " << msg.str();
  }
  notify();
  return false;
}

ResultT<TRI_voc_tick_t> SynchronizeShard::catchupWithReadLock(
    std::string const& ep, std::string const& database, LogicalCollection const& collection,
    std::string const& clientId, std::string const& shard,
    std::string const& leader, TRI_voc_tick_t lastLogTick, VPackBuilder& builder) {
  bool didTimeout = true;
  int tries = 0;
  double timeout = 300.0;
  TRI_voc_tick_t tickReached = 0;
  while (didTimeout && tries++ < 18) {  // This will try to sync for at most 1 hour. (200 * 18 == 3600)

    if (isStopping()) {
      std::string errorMessage =
          "synchronizeOneShard: startReadLockOnLeader (soft): shutting down";
      return ResultT<TRI_voc_tick_t>::error(TRI_ERROR_SHUTTING_DOWN, errorMessage);
    }

    didTimeout = false;
    // Now ask for a "soft stop" on the leader, in case of mmfiles, this
    // will be a hard stop, but for rocksdb, this is a no-op:
    uint64_t lockJobId = 0;
    LOG_TOPIC("b4f2b", DEBUG, Logger::MAINTENANCE)
        << "synchronizeOneShard: startReadLockOnLeader (soft): " << ep << ":"
        << database << ":" << collection.name();
    Result res = startReadLockOnLeader(ep, database, collection.name(),
                                       clientId, lockJobId, true, timeout);
    if (!res.ok()) {
      std::string errorMessage =
          "synchronizeOneShard: error in startReadLockOnLeader (soft):" + res.errorMessage();
      return ResultT<TRI_voc_tick_t>::error(TRI_ERROR_INTERNAL, errorMessage);
    }

    auto readLockGuard = arangodb::scopeGuard([&, this]() {
      // Always cancel the read lock.
      // Reported seperately
      NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
      network::ConnectionPool* pool = nf.pool();
      auto res = cancelReadLockOnLeader(pool, ep, database, lockJobId, clientId, 60.0);
      if (!res.ok()) {
        LOG_TOPIC("b15ee", INFO, Logger::MAINTENANCE)
            << "Could not cancel soft read lock on leader: " << res.errorMessage();
      }
    });

    LOG_TOPIC("5eb37", DEBUG, Logger::MAINTENANCE) << "lockJobId: " << lockJobId;

    // From now on, we need to cancel the read lock on the leader regardless
    // if things go wrong or right!

    // Do a first try of a catch up with the WAL. In case of RocksDB,
    // this has not yet stopped the writes, so we have to be content
    // with nearly reaching the end of the WAL, which is a "soft" catchup.
    builder.clear();
    {
      VPackObjectBuilder o(&builder);
      builder.add(ENDPOINT, VPackValue(ep));
      builder.add(DATABASE, VPackValue(database));
      builder.add(COLLECTION, VPackValue(shard));
      builder.add(LEADER_ID, VPackValue(leader));
      builder.add("from", VPackValue(lastLogTick));
      builder.add("requestTimeout", VPackValue(600.0));
      builder.add("connectTimeout", VPackValue(60.0));
    }

    // We only allow to hold this lock for 60% of the timeout time, so to avoid
    // any issues with Locks timeouting on the Leader and the Client not
    // recognizing it.
    res = replicationSynchronizeCatchup(builder.slice(), timeout * 0.6, tickReached, didTimeout);

    if (!res.ok()) {
      std::string errorMessage(
          "synchronizeOneshard: error in syncCollectionCatchup: ");
      errorMessage += res.errorMessage();
      return ResultT<TRI_voc_tick_t>::error(TRI_ERROR_INTERNAL, errorMessage);
    }

    // Stop the read lock again:
    NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
    network::ConnectionPool* pool = nf.pool();
    res = cancelReadLockOnLeader(pool, ep, database, lockJobId, clientId, 60.0);
    // We removed the readlock
    readLockGuard.cancel();
    if (!res.ok()) {
      std::string errorMessage =
          "synchronizeOneShard: error when cancelling soft read lock: " + res.errorMessage();
      LOG_TOPIC("c37d1", INFO, Logger::MAINTENANCE) << errorMessage;
      _result.reset(TRI_ERROR_INTERNAL, errorMessage);
      return ResultT<TRI_voc_tick_t>::error(TRI_ERROR_INTERNAL, errorMessage);
    }
    lastLogTick = tickReached;
    if (didTimeout) {
      LOG_TOPIC("e516e", INFO, Logger::MAINTENANCE)
          << "Renewing softLock for " << shard << " on leader: " << leader;
    }
  }
  if (didTimeout) {
    LOG_TOPIC("f1a61", WARN, Logger::MAINTENANCE)
        << "Could not catchup under softLock for " << shard << " on leader: " << leader
        << " now activating hardLock. This is expected under high load.";
  }
  return ResultT<TRI_voc_tick_t>::success(tickReached);
}

Result SynchronizeShard::catchupWithExclusiveLock(
    std::string const& ep, std::string const& database, LogicalCollection const& collection,
    std::string const& clientId, std::string const& shard, std::string const& leader,
    SyncerId const syncerId, TRI_voc_tick_t lastLogTick, VPackBuilder& builder) {
  uint64_t lockJobId = 0;
  LOG_TOPIC("da129", DEBUG, Logger::MAINTENANCE)
      << "synchronizeOneShard: startReadLockOnLeader: " << ep << ":" << database
      << ":" << collection.name();
  Result res = startReadLockOnLeader(ep, database, collection.name(), clientId,
                                     lockJobId, false);
  if (!res.ok()) {
    std::string errorMessage =
        "synchronizeOneShard: error in startReadLockOnLeader (hard):" + res.errorMessage();
    return {TRI_ERROR_INTERNAL, errorMessage};
  }
  auto readLockGuard = arangodb::scopeGuard([&, this]() {
    // Always cancel the read lock.
    // Reported seperately
    NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
    network::ConnectionPool* pool = nf.pool();
    auto res = cancelReadLockOnLeader(pool, ep, database, lockJobId, clientId, 60.0);
    if (!res.ok()) {
      LOG_TOPIC("067a8", INFO, Logger::MAINTENANCE)
          << "Could not cancel hard read lock on leader: " << res.errorMessage();
    }
  });

  LOG_TOPIC("d76cb", DEBUG, Logger::MAINTENANCE) << "lockJobId: " << lockJobId;

  builder.clear();
  {
    VPackObjectBuilder o(&builder);
    builder.add(ENDPOINT, VPackValue(ep));
    builder.add(DATABASE, VPackValue(database));
    builder.add(COLLECTION, VPackValue(shard));
    builder.add(LEADER_ID, VPackValue(leader));
    builder.add("from", VPackValue(lastLogTick));
    builder.add("requestTimeout", VPackValue(600.0));
    builder.add("connectTimeout", VPackValue(60.0));
  }

  res = replicationSynchronizeFinalize(builder.slice());

  if (!res.ok()) {
    std::string errorMessage(
        "synchronizeOneshard: error in syncCollectionFinalize: ");
    errorMessage += res.errorMessage();
    return {TRI_ERROR_INTERNAL, errorMessage};
  }

  NetworkFeature& nf = _feature.server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  res = addShardFollower(pool, ep, database, shard, lockJobId, clientId,
                         syncerId, _clientInfoString, 60.0);

  if (!res.ok()) {
    std::string errorMessage(
        "synchronizeOneshard: error in addShardFollower: ");
    errorMessage += res.errorMessage();
    return {TRI_ERROR_INTERNAL, errorMessage};
  }

  // Report success:
  LOG_TOPIC("3423d", DEBUG, Logger::MAINTENANCE)
      << "synchronizeOneShard: synchronization worked for shard " << shard;
  _result.reset(TRI_ERROR_NO_ERROR);
  return {TRI_ERROR_NO_ERROR};
}

void SynchronizeShard::setState(ActionState state) {
  if ((COMPLETE == state || FAILED == state) && _state != state) {
    TRI_ASSERT(_description.has("shard"));
    _feature.incShardVersion(_description.get("shard"));
  }

  ActionBase::setState(state);
}
