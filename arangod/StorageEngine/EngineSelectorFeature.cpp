////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "EngineSelectorFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/FileUtils.h"
#include "Basics/StringUtils.h"
#include "Basics/application-exit.h"
#include "Cluster/ServerState.h"
#include "ClusterEngine/ClusterEngine.h"
#include "FeaturePhases/BasicFeaturePhaseServer.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "MMFiles/MMFilesEngine.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "RestServer/DatabasePathFeature.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "StorageEngine/StorageEngine.h"

using namespace arangodb::options;

namespace {
std::unordered_map<std::string, std::type_index> createEngineMap() {
  std::unordered_map<std::string, std::type_index> map;
  map.try_emplace(arangodb::MMFilesEngine::EngineName,
              std::type_index(typeid(arangodb::MMFilesEngine)));
  map.try_emplace(arangodb::RocksDBEngine::EngineName,
              std::type_index(typeid(arangodb::RocksDBEngine)));
  return map;
}
}

namespace arangodb {

StorageEngine* EngineSelectorFeature::ENGINE = nullptr;

EngineSelectorFeature::EngineSelectorFeature(application_features::ApplicationServer& server)
    : ApplicationFeature(server, "EngineSelector"), _engine("auto"), _selected(false) {
  setOptional(false);
  startsAfter<application_features::BasicFeaturePhaseServer>();

  auto map = ::createEngineMap();
}

void EngineSelectorFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("server", "Server features");

  options->addOption("--server.storage-engine", "storage engine type",
                     new DiscreteValuesParameter<StringParameter>(&_engine, availableEngineNames()));
}

void EngineSelectorFeature::prepare() {
#ifdef ARANGODB_USE_GOOGLE_TESTS
  if (_selected.load()) {
    // already set in the test code
    return;
  }
#endif
  // read engine from file in database_directory ENGINE (mmfiles/rocksdb)
  auto& databasePathFeature = server().getFeature<DatabasePathFeature>();
  auto path = databasePathFeature.directory();
  _engineFilePath = basics::FileUtils::buildFilename(path, "ENGINE");

  // fail if engine value in file does not match command-line option
  if (!ServerState::instance()->isCoordinator() &&
      basics::FileUtils::isRegularFile(_engineFilePath)) {
    LOG_TOPIC("98b5c", DEBUG, Logger::STARTUP)
        << "looking for previously selected engine in file '" << _engineFilePath << "'";
    try {
      std::string content =
          basics::StringUtils::trim(basics::FileUtils::slurp(_engineFilePath));
      if (content != _engine && _engine != "auto") {
        LOG_TOPIC("cd6d8", FATAL, Logger::STARTUP)
            << "content of 'ENGINE' file '" << _engineFilePath
            << "' and command-line/configuration option value do not match: '"
            << content << "' != '" << _engine
            << "'. please validate the command-line/configuration option value "
               "of '--server.storage-engine' or use a different database "
               "directory if the change is intentional";
        FATAL_ERROR_EXIT();
      }
      _engine = content;
    } catch (std::exception const& ex) {
      LOG_TOPIC("23ec1", FATAL, Logger::STARTUP)
          << "unable to read content of 'ENGINE' file '" << _engineFilePath
          << "': " << ex.what()
          << ". please make sure the file/directory is readable for the "
             "arangod process and user";
      FATAL_ERROR_EXIT();
    }
  }

  if (_engine == "auto") {
    _engine = defaultEngine();
  }

  TRI_ASSERT(_engine != "auto");

  if (ServerState::instance()->isCoordinator()) {
    ClusterEngine& ce = server().getFeature<ClusterEngine>();
    ENGINE = &ce;

    for (auto const& engine : availableEngines()) {
      StorageEngine& e = server().getFeature<StorageEngine>(engine.second);
      // turn off all other storage engines
      LOG_TOPIC("001b6", TRACE, Logger::STARTUP) << "disabling storage engine " << engine.first;
      e.disable();
      if (engine.first == _engine) {
        LOG_TOPIC("4a3fc", INFO, Logger::FIXME) << "using storage engine " << engine.first;
        ce.setActualEngine(&e);
      }
    }

  } else {
    // deactivate all engines but the selected one
    for (auto engine : availableEngines()) {
      auto& e = server().getFeature<StorageEngine>(engine.second);

      if (engine.first == _engine) {
        // this is the selected engine
        LOG_TOPIC("144fe", INFO, Logger::FIXME)
            << "using storage engine '" << engine.first << "'";
        e.enable();

        // register storage engine
        TRI_ASSERT(ENGINE == nullptr);
        ENGINE = &e;
      } else {
        // turn off all other storage engines
        LOG_TOPIC("14a9e", TRACE, Logger::STARTUP)
            << "disabling storage engine '" << engine.first << "'";
        e.disable();
      }
    }
  }

  if (ENGINE == nullptr) {
    LOG_TOPIC("9cb11", FATAL, Logger::STARTUP)
        << "unable to figure out storage engine from selection '" << _engine
        << "'. please use the '--server.storage-engine' option to select an "
           "existing storage engine";
    FATAL_ERROR_EXIT();
  }

  _selected.store(true);
}

void EngineSelectorFeature::start() {
  TRI_ASSERT(ENGINE != nullptr);

  // write engine File
  if (!ServerState::instance()->isCoordinator() &&
      !basics::FileUtils::isRegularFile(_engineFilePath)) {
    try {
      basics::FileUtils::spit(_engineFilePath, _engine, true);
    } catch (std::exception const& ex) {
      LOG_TOPIC("4ff0f", FATAL, Logger::STARTUP)
          << "unable to write 'ENGINE' file '" << _engineFilePath << "': " << ex.what()
          << ". please make sure the file/directory is writable for the "
             "arangod process and user";
      FATAL_ERROR_EXIT();
    }
  }
}

void EngineSelectorFeature::unprepare() {
  // unregister storage engine
  _selected.store(false);
  ENGINE = nullptr;

  if (ServerState::instance()->isCoordinator()) {
    ClusterEngine& ce = server().getFeature<ClusterEngine>();
    ce.setActualEngine(nullptr);
  }
}

// return the names of all available storage engines
std::unordered_set<std::string> EngineSelectorFeature::availableEngineNames() {
  std::unordered_set<std::string> result;
  for (auto const& it : availableEngines()) {
    result.emplace(it.first);
  }
  result.emplace("auto");
  return result;
}

// return all available storage engines
std::unordered_map<std::string, std::type_index> EngineSelectorFeature::availableEngines() {
  return ::createEngineMap();
}

StorageEngine& EngineSelectorFeature::engine() {
  if (!selected()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
  }
  return *ENGINE;
}

std::string const& EngineSelectorFeature::engineName() {
  return ENGINE->typeName();
}

std::string const& EngineSelectorFeature::defaultEngine() {
  return RocksDBEngine::EngineName;
}

bool EngineSelectorFeature::isMMFiles() {
  return engineName() == MMFilesEngine::EngineName;
}

bool EngineSelectorFeature::isRocksDB() {
  return engineName() == RocksDBEngine::EngineName;
}

#ifdef ARANGODB_USE_GOOGLE_TESTS
void EngineSelectorFeature::setEngineTesting(StorageEngine* input) {
  TRI_ASSERT((input == nullptr) != (ENGINE == nullptr));
  _selected.store(input != nullptr);
  ENGINE = input;
}
#endif

}  // namespace arangodb
