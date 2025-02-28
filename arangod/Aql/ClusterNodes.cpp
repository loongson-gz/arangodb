////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
////////////////////////////////////////////////////////////////////////////////

#include <type_traits>

#include <velocypack/Iterator.h>
#include <velocypack/StringRef.h>
#include <velocypack/velocypack-aliases.h>

#include "ClusterNodes.h"

#include "Aql/AqlValue.h"
#include "Aql/Ast.h"
#include "Aql/BlocksWithClients.h"
#include "Aql/Collection.h"
#include "Aql/DistributeExecutor.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/ExecutorInfos.h"
#include "Aql/GraphNode.h"
#include "Aql/IdExecutor.h"
#include "Aql/IndexNode.h"
#include "Aql/ModificationNodes.h"
#include "Aql/MultiDependencySingleRowFetcher.h"
#include "Aql/Query.h"
#include "Aql/RemoteExecutor.h"
#include "Aql/ScatterExecutor.h"
#include "Aql/SingleRemoteModificationExecutor.h"
#include "Aql/SortRegister.h"
#include "Aql/SortingGatherExecutor.h"
#include "Aql/UnsortedGatherExecutor.h"
#include "Aql/UnsortingGatherExecutor.h"
#include "Aql/types.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ServerState.h"
#include "Logger/LogMacros.h"
#include "Transaction/Methods.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::aql;

namespace {

arangodb::velocypack::StringRef const SortModeUnset("unset");
arangodb::velocypack::StringRef const SortModeMinElement("minelement");
arangodb::velocypack::StringRef const SortModeHeap("heap");

char const* toString(GatherNode::Parallelism value) {
  switch (value) {
    case GatherNode::Parallelism::Parallel:
      return "parallel";
    case GatherNode::Parallelism::Serial:
      return "serial";
    case GatherNode::Parallelism::Undefined:
    default:
      return "undefined";
  }
}

GatherNode::Parallelism parallelismFromString(std::string const& value) {
  if (value == "parallel") {
    return GatherNode::Parallelism::Parallel;
  } else if (value == "serial") {
    return GatherNode::Parallelism::Serial;
  }
  return GatherNode::Parallelism::Undefined;
}

std::map<arangodb::velocypack::StringRef, GatherNode::SortMode> const NameToValue{
    {SortModeMinElement, GatherNode::SortMode::MinElement},
    {SortModeHeap, GatherNode::SortMode::Heap},
    {SortModeUnset, GatherNode::SortMode::Default}};

bool toSortMode(arangodb::velocypack::StringRef const& str, GatherNode::SortMode& mode) noexcept {
  // std::map ~25-30% faster than std::unordered_map for small number of elements
  auto const it = NameToValue.find(str);

  if (it == NameToValue.end()) {
    TRI_ASSERT(false);
    return false;
  }

  mode = it->second;
  return true;
}

arangodb::velocypack::StringRef toString(GatherNode::SortMode mode) noexcept {
  switch (mode) {
    case GatherNode::SortMode::MinElement:
      return SortModeMinElement;
    case GatherNode::SortMode::Heap:
      return SortModeHeap;
    case GatherNode::SortMode::Default:
      return SortModeUnset;
    default:
      TRI_ASSERT(false);
      return {};
  }
}

}  // namespace

/// @brief constructor for RemoteNode
RemoteNode::RemoteNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : DistributeConsumerNode(plan, base),
      _vocbase(&(plan->getAst()->query()->vocbase())),
      _server(base.get("server").copyString()),
      _queryId(base.get("queryId").copyString()) {
  // Backwards compatibility (3.4.x)(3.5.0) and earlier, coordinator might send ownName.
  arangodb::velocypack::StringRef tmpId(getDistributeId());
  tmpId = VelocyPackHelper::getStringRef(base, "ownName", tmpId);
  if (tmpId != getDistributeId()) {
    setDistributeId(tmpId.toString());
  }
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> RemoteNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  RegisterId const nrOutRegs = getRegisterPlan()->nrRegs[getDepth()];
  RegisterId const nrInRegs = nrOutRegs;

  std::unordered_set<RegisterId> regsToKeep{};
  {  // This block sets regsToKeep.
    // It's essentially a copy of ExecutionNode::calcRegsToKeep(), but it
    // doesn't use the previous node, which we do not have here.
    regsToKeep.reserve(getVarsUsedLater().size());
    std::unordered_map<VariableId, VarInfo> const& varInfo = getRegisterPlan()->varInfo;
    for (auto const var : getVarsUsedLater()) {
      auto const it = varInfo.find(var->id);
      TRI_ASSERT(it != varInfo.end());
      RegisterId const reg = it->second.registerId;
      if (reg < nrInRegs) {
        bool inserted;
        std::tie(std::ignore, inserted) = regsToKeep.emplace(reg);
        TRI_ASSERT(inserted);
      }
    }
  }

  std::unordered_set<RegisterId> regsToClear = getRegsToClear();

  // Everything that is cleared here could and should have been cleared before,
  // i.e. before sending it over the network.
  TRI_ASSERT(regsToClear.empty());

  ExecutorInfos infos({}, {}, nrInRegs, nrOutRegs, std::move(regsToClear),
                      std::move(regsToKeep));

  return std::make_unique<ExecutionBlockImpl<RemoteExecutor>>(
      &engine, this, std::move(infos), server(), getDistributeId(), queryId());
}

/// @brief toVelocyPack, for RemoteNode
void RemoteNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                    std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  DistributeConsumerNode::toVelocyPackHelperInternal(nodes, flags, seen);

  nodes.add("database", VPackValue(_vocbase->name()));
  nodes.add("server", VPackValue(_server));
  nodes.add("queryId", VPackValue(_queryId));

  // And close it:
  nodes.close();
}

/// @brief estimateCost
CostEstimate RemoteNode::estimateCost() const {
  if (_dependencies.size() == 1) {
    CostEstimate estimate = _dependencies[0]->getCost();
    estimate.estimatedCost += estimate.estimatedNrItems;
    return estimate;
  }
  // We really should not get here, but if so, do something bordering on
  // sensible:
  CostEstimate estimate = CostEstimate::empty();
  estimate.estimatedNrItems = 1;
  estimate.estimatedCost = 1.0;
  return estimate;
}

/// @brief construct a scatter node
ScatterNode::ScatterNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base) {
  readClientsFromVelocyPack(base);
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> ScatterNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId const nrOutRegs = getRegisterPlan()->nrRegs[getDepth()];
  RegisterId const nrInRegs = getRegisterPlan()->nrRegs[previousNode->getDepth()];

  std::unordered_set<RegisterId> regsToKeep = calcRegsToKeep();
  std::unordered_set<RegisterId> regsToClear = getRegsToClear();

  ExecutorInfos infos({}, {}, nrInRegs, nrOutRegs, std::move(regsToClear),
                      std::move(regsToKeep));
  return std::make_unique<ExecutionBlockImpl<ScatterExecutor>>(&engine, this,
                                                               std::move(infos), _clients);
}

/// @brief toVelocyPack, for ScatterNode
void ScatterNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                     std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);

  // serialize clients
  writeClientsToVelocyPack(nodes);
  // And close it:
  nodes.close();
}

bool ScatterNode::readClientsFromVelocyPack(VPackSlice base) {
  auto const clientsSlice = base.get("clients");

  if (!clientsSlice.isArray()) {
    LOG_TOPIC("49ba1", ERR, Logger::AQL)
        << "invalid serialized ScatterNode definition, 'clients' attribute is "
           "expected to be an array of string";
    return false;
  }

  size_t pos = 0;
  for (auto const clientSlice : velocypack::ArrayIterator(clientsSlice)) {
    if (!clientSlice.isString()) {
      LOG_TOPIC("c6131", ERR, Logger::AQL)
          << "invalid serialized ScatterNode definition, 'clients' attribute "
             "is expected to be an array of string but got not a string at "
             "line "
          << pos;
      _clients.clear();  // clear malformed node
      return false;
    }

    _clients.emplace_back(clientSlice.copyString());
    ++pos;
  }

  _type = static_cast<ScatterNode::ScatterType>(
      basics::VelocyPackHelper::getNumericValue<uint64_t>(base, "scatterType", 0));

  return true;
}

void ScatterNode::writeClientsToVelocyPack(VPackBuilder& builder) const {
  builder.add("scatterType", VPackValue(static_cast<uint64_t>(getScatterType())));
  VPackArrayBuilder arrayScope(&builder, "clients");
  for (auto const& client : _clients) {
    builder.add(VPackValue(client));
  }
}

/// @brief estimateCost
CostEstimate ScatterNode::estimateCost() const {
  CostEstimate estimate = _dependencies[0]->getCost();
  estimate.estimatedCost += estimate.estimatedNrItems * _clients.size();
  return estimate;
}

/// @brief construct a distribute node
DistributeNode::DistributeNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ScatterNode(plan, base),
      CollectionAccessingNode(plan, base),
      _variable(nullptr),
      _alternativeVariable(nullptr),
      _createKeys(base.get("createKeys").getBoolean()),
      _allowKeyConversionToObject(base.get("allowKeyConversionToObject").getBoolean()),
      _allowSpecifiedKeys(false) {
  if (base.hasKey("variable") && base.hasKey("alternativeVariable")) {
    _variable = Variable::varFromVPack(plan->getAst(), base, "variable");
    _alternativeVariable =
        Variable::varFromVPack(plan->getAst(), base, "alternativeVariable");
  } else {
    _variable = plan->getAst()->variables()->getVariable(
        base.get("varId").getNumericValue<VariableId>());
    _alternativeVariable = plan->getAst()->variables()->getVariable(
        base.get("alternativeVarId").getNumericValue<VariableId>());
  }
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> DistributeNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId const nrOutRegs = getRegisterPlan()->nrRegs[getDepth()];
  RegisterId const nrInRegs = getRegisterPlan()->nrRegs[previousNode->getDepth()];

  std::unordered_set<RegisterId> regsToKeep = calcRegsToKeep();
  std::unordered_set<RegisterId> regsToClear = getRegsToClear();

  ExecutorInfos infos({}, {}, nrInRegs, nrOutRegs, std::move(regsToClear),
                      std::move(regsToKeep));

  RegisterId regId;
  RegisterId alternativeRegId = RegisterPlan::MaxRegisterId;

  {  // set regId and alternativeRegId:

    // get the variable to inspect . . .
    VariableId varId = _variable->id;

    // get the register id of the variable to inspect . . .
    auto it = getRegisterPlan()->varInfo.find(varId);
    TRI_ASSERT(it != getRegisterPlan()->varInfo.end());
    regId = (*it).second.registerId;

    TRI_ASSERT(regId < RegisterPlan::MaxRegisterId);

    if (_alternativeVariable != _variable) {
      // use second variable
      auto it = getRegisterPlan()->varInfo.find(_alternativeVariable->id);
      TRI_ASSERT(it != getRegisterPlan()->varInfo.end());
      alternativeRegId = (*it).second.registerId;

      TRI_ASSERT(alternativeRegId < RegisterPlan::MaxRegisterId);
    } else {
      TRI_ASSERT(alternativeRegId == RegisterPlan::MaxRegisterId);
    }
  }

  return std::make_unique<ExecutionBlockImpl<DistributeExecutor>>(
      &engine, this, std::move(infos), clients(), collection(), regId, alternativeRegId,
      _allowSpecifiedKeys, _allowKeyConversionToObject, _createKeys);
}

/// @brief toVelocyPack, for DistributedNode
void DistributeNode::toVelocyPackHelper(VPackBuilder& builder, unsigned flags,
                                        std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(builder, flags, seen);

  // add collection information
  CollectionAccessingNode::toVelocyPack(builder, flags);

  // serialize clients
  writeClientsToVelocyPack(builder);

  builder.add("createKeys", VPackValue(_createKeys));
  builder.add("allowKeyConversionToObject", VPackValue(_allowKeyConversionToObject));
  builder.add(VPackValue("variable"));
  _variable->toVelocyPack(builder);
  builder.add(VPackValue("alternativeVariable"));
  _alternativeVariable->toVelocyPack(builder);

  // legacy format, remove in 3.4
  builder.add("varId", VPackValue(static_cast<int>(_variable->id)));
  builder.add("alternativeVarId",
              VPackValue(static_cast<int>(_alternativeVariable->id)));

  // And close it:
  builder.close();
}

/// @brief getVariablesUsedHere, modifying the set in-place
void DistributeNode::getVariablesUsedHere(::arangodb::containers::HashSet<Variable const*>& vars) const {
  vars.emplace(_variable);
  vars.emplace(_alternativeVariable);
}

/// @brief estimateCost
CostEstimate DistributeNode::estimateCost() const {
  CostEstimate estimate = _dependencies[0]->getCost();
  estimate.estimatedCost += estimate.estimatedNrItems;
  return estimate;
}

/*static*/ Collection const* GatherNode::findCollection(GatherNode const& root) noexcept {
  ExecutionNode const* node = root.getFirstDependency();

  while (node) {
    switch (node->getType()) {
      case ENUMERATE_COLLECTION:
        return castTo<EnumerateCollectionNode const*>(node)->collection();
      case INDEX:
        return castTo<IndexNode const*>(node)->collection();
      case TRAVERSAL:
      case SHORTEST_PATH:
      case K_SHORTEST_PATHS:
        return castTo<GraphNode const*>(node)->collection();
      case SCATTER:
        return nullptr;  // diamond boundary
      default:
        node = node->getFirstDependency();
        break;
    }
  }

  return nullptr;
}

/// @brief construct a gather node
GatherNode::GatherNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base,
                       SortElementVector const& elements)
    : ExecutionNode(plan, base),
      _elements(elements),
      _sortmode(SortMode::MinElement),
      _parallelism(Parallelism::Undefined), 
      _limit(0) {
  if (!_elements.empty()) {
    auto const sortModeSlice = base.get("sortmode");

    if (!toSortMode(VelocyPackHelper::getStringRef(sortModeSlice, VPackStringRef()), _sortmode)) {
      LOG_TOPIC("2c6f3", ERR, Logger::AQL)
          << "invalid sort mode detected while "
             "creating 'GatherNode' from vpack";
    }

    _limit =
        basics::VelocyPackHelper::getNumericValue<decltype(_limit)>(base,
                                                                    "limit", 0);
  }
    
  setParallelism(parallelismFromString(VelocyPackHelper::getStringValue(base, "parellelism", "")));
}

GatherNode::GatherNode(ExecutionPlan* plan, size_t id, SortMode sortMode, Parallelism parallelism) noexcept
    : ExecutionNode(plan, id), 
      _sortmode(sortMode), 
      _parallelism(parallelism),
      _limit(0) {}

/// @brief toVelocyPack, for GatherNode
void GatherNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                    std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);
 
  nodes.add("parallelism", VPackValue(toString(_parallelism)));

  if (_elements.empty()) {
    nodes.add("sortmode", VPackValue(SortModeUnset.data()));
  } else {
    nodes.add("sortmode", VPackValue(toString(_sortmode).data()));
    nodes.add("limit", VPackValue(_limit));
  }

  nodes.add(VPackValue("elements"));
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& it : _elements) {
      VPackObjectBuilder obj(&nodes);
      nodes.add(VPackValue("inVariable"));
      it.var->toVelocyPack(nodes);
      nodes.add("ascending", VPackValue(it.ascending));
      if (!it.attributePath.empty()) {
        nodes.add(VPackValue("path"));
        VPackArrayBuilder arr(&nodes);
        for (auto const& a : it.attributePath) {
          nodes.add(VPackValue(a));
        }
      }
    }
  }
  
  // And close it:
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> GatherNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);
  if (_elements.empty()) {
    TRI_ASSERT(getRegisterPlan()->nrRegs[previousNode->getDepth()] ==
               getRegisterPlan()->nrRegs[getDepth()]);
    if (ServerState::instance()->isCoordinator() && _parallelism == Parallelism::Parallel) {
      UnsortedGatherExecutorInfos infos(getRegisterPlan()->nrRegs[getDepth()],
                                        calcRegsToKeep(), getRegsToClear());
      return std::make_unique<ExecutionBlockImpl<UnsortedGatherExecutor>>(&engine, this, std::move(infos));
    } else {
      IdExecutorInfos infos(getRegisterPlan()->nrRegs[getDepth()],
                            calcRegsToKeep(), getRegsToClear());

      return std::make_unique<ExecutionBlockImpl<UnsortingGatherExecutor>>(&engine, this,
                                                                           std::move(infos));
    }
  }
  
  Parallelism p = _parallelism;
  if (ServerState::instance()->isDBServer()) {
    p = Parallelism::Serial; // not supported in v36
  }
  
  std::vector<SortRegister> sortRegister;
  SortRegister::fill(*plan(), *getRegisterPlan(), _elements, sortRegister);
  SortingGatherExecutorInfos infos(make_shared_unordered_set(),
                                   make_shared_unordered_set(),
                                   getRegisterPlan()->nrRegs[previousNode->getDepth()],
                                   getRegisterPlan()->nrRegs[getDepth()], getRegsToClear(),
                                   calcRegsToKeep(), std::move(sortRegister),
                                   _plan->getAst()->query()->trx(), sortMode(),
                                   constrainedSortLimit(), p);

  return std::make_unique<ExecutionBlockImpl<SortingGatherExecutor>>(&engine, this,
                                                                     std::move(infos));
}

/// @brief estimateCost
CostEstimate GatherNode::estimateCost() const {
  CostEstimate estimate = _dependencies[0]->getCost();
  estimate.estimatedCost += estimate.estimatedNrItems;
  return estimate;
}

void GatherNode::setConstrainedSortLimit(size_t limit) noexcept {
  _limit = limit;
}

size_t GatherNode::constrainedSortLimit() const noexcept { return _limit; }

bool GatherNode::isSortingGather() const noexcept {
  return !elements().empty();
}

/// @brief is the node parallelizable?
struct ParallelizableFinder final : public WalkerWorker<ExecutionNode> {
  bool _isParallelizable = true;

  ParallelizableFinder() : _isParallelizable(true) {}
  ~ParallelizableFinder() = default;

  bool enterSubquery(ExecutionNode*, ExecutionNode*) override final {
    return false;
  }

  bool before(ExecutionNode* node) override final {
    if (node->getType() == ExecutionNode::SCATTER ||
        node->getType() == ExecutionNode::GATHER ||
        node->getType() == ExecutionNode::DISTRIBUTE) {
      _isParallelizable = false;
      return true;  // true to abort the whole walking process
    }
    if (node->isModificationNode()) {
        /*
         * TODO: enable parallelization for REMOVE, REPLACE, UPDATE
         * as well. This seems safe as long as there is no DistributeNode
         * and there is no further communication using Scatter/Gather.
         * But this needs more testing first
        && 
        (node->getType() != ExecutionNode::REMOVE &&
         node->getType() != ExecutionNode::REPLACE && 
         node->getType() != ExecutionNode::UPDATE)) {
         */
      // REMOVEs and REPLACEs are actually parallelizable, as they are completely independent
      // from each other on different shards
      _isParallelizable = false;
      return true;  // true to abort the whole walking process
    }

    // continue inspecting
    return false;
  }
};

/// no modification nodes, ScatterNodes etc
bool GatherNode::isParallelizable() const {
  if (_parallelism == Parallelism::Serial) {
    // node already defined to be serial
    return false;
  }

  ParallelizableFinder finder;
  for (ExecutionNode* e : _dependencies) {
    e->walk(finder);
    if (!finder._isParallelizable) {
      return false;
    }
  }
  return true;
}

void GatherNode::setParallelism(GatherNode::Parallelism value) {
  TRI_ASSERT(value != Parallelism::Parallel || isParallelizable());
  _parallelism = value;
}

SingleRemoteOperationNode::SingleRemoteOperationNode(
    ExecutionPlan* plan, size_t id, NodeType mode, bool replaceIndexNode,
    std::string const& key, Collection const* collection,
    ModificationOptions const& options, Variable const* in, Variable const* out,
    Variable const* OLD, Variable const* NEW)
    : ExecutionNode(plan, id),
      CollectionAccessingNode(collection),
      _replaceIndexNode(replaceIndexNode),
      _key(key),
      _mode(mode),
      _inVariable(in),
      _outVariable(out),
      _outVariableOld(OLD),
      _outVariableNew(NEW),
      _options(options) {
  if (_mode == NodeType::INDEX) {  // select
    TRI_ASSERT(!_key.empty());
    TRI_ASSERT(_inVariable == nullptr);
    TRI_ASSERT(_outVariable != nullptr);
    TRI_ASSERT(_outVariableOld == nullptr);
    TRI_ASSERT(_outVariableNew == nullptr);
  } else if (_mode == NodeType::REMOVE) {
    TRI_ASSERT(!_key.empty());
    TRI_ASSERT(_inVariable == nullptr);
    TRI_ASSERT(_outVariableNew == nullptr);
  } else if (_mode == NodeType::INSERT) {
    TRI_ASSERT(_key.empty());
  } else if (_mode != NodeType::UPDATE && _mode != NodeType::REPLACE) {
    TRI_ASSERT(false);
  }
}

/// @brief creates corresponding SingleRemoteOperationNode
std::unique_ptr<ExecutionBlock> SingleRemoteOperationNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();

  TRI_ASSERT(previousNode != nullptr);

  RegisterId in = variableToRegisterOptionalId(_inVariable);
  RegisterId out = variableToRegisterOptionalId(_outVariable);
  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);
  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  OperationOptions options =
      ModificationExecutorHelpers::convertOptions(_options, _outVariableNew, _outVariableOld);

  SingleRemoteModificationInfos infos(
      in, outputNew, outputOld, out,
      getRegisterPlan()->nrRegs[previousNode->getDepth()] /*nr input regs*/,
      getRegisterPlan()->nrRegs[getDepth()] /*nr output regs*/, getRegsToClear(),
      calcRegsToKeep(), _plan->getAst()->query()->trx(), std::move(options),
      _collection, ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors),
      IgnoreDocumentNotFound(_options.ignoreDocumentNotFound), _key,
      this->hasParent(), this->_replaceIndexNode);

  if (_mode == NodeType::INDEX) {
    return std::make_unique<ExecutionBlockImpl<SingleRemoteModificationExecutor<IndexTag>>>(
        &engine, this, std::move(infos));
  } else if (_mode == NodeType::INSERT) {
    return std::make_unique<ExecutionBlockImpl<SingleRemoteModificationExecutor<Insert>>>(
        &engine, this, std::move(infos));
  } else if (_mode == NodeType::REMOVE) {
    return std::make_unique<ExecutionBlockImpl<SingleRemoteModificationExecutor<Remove>>>(
        &engine, this, std::move(infos));
  } else if (_mode == NodeType::REPLACE) {
    return std::make_unique<ExecutionBlockImpl<SingleRemoteModificationExecutor<Replace>>>(
        &engine, this, std::move(infos));
  } else if (_mode == NodeType::UPDATE) {
    return std::make_unique<ExecutionBlockImpl<SingleRemoteModificationExecutor<Update>>>(
        &engine, this, std::move(infos));
  } else if (_mode == NodeType::UPSERT) {
    return std::make_unique<ExecutionBlockImpl<SingleRemoteModificationExecutor<Upsert>>>(
        &engine, this, std::move(infos));
  } else {
    TRI_ASSERT(false);
    return nullptr;
  }
}

/// @brief toVelocyPack, for SingleRemoteOperationNode
void SingleRemoteOperationNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                                   std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);
  CollectionAccessingNode::toVelocyPackHelperPrimaryIndex(nodes);

  // add collection information
  CollectionAccessingNode::toVelocyPack(nodes, flags);

  nodes.add("mode", VPackValue(ExecutionNode::getTypeString(_mode)));
  nodes.add("replaceIndexNode", VPackValue(_replaceIndexNode));

  if (!_key.empty()) {
    nodes.add("key", VPackValue(_key));
  }

  // add out variables
  bool isAnyVarUsedLater = false;
  if (_outVariableOld != nullptr) {
    nodes.add(VPackValue("outVariableOld"));
    _outVariableOld->toVelocyPack(nodes);
    isAnyVarUsedLater |= isVarUsedLater(_outVariableOld);
  }
  if (_outVariableNew != nullptr) {
    nodes.add(VPackValue("outVariableNew"));
    _outVariableNew->toVelocyPack(nodes);
    isAnyVarUsedLater |= isVarUsedLater(_outVariableNew);
  }

  if (_inVariable != nullptr) {
    nodes.add(VPackValue("inVariable"));
    _inVariable->toVelocyPack(nodes);
  }

  if (_outVariable != nullptr) {
    nodes.add(VPackValue("outVariable"));
    _outVariable->toVelocyPack(nodes);
    isAnyVarUsedLater |= isVarUsedLater(_outVariable);
  }
  nodes.add("producesResult", VPackValue(isAnyVarUsedLater));
  nodes.add(VPackValue("modificationFlags"));
  _options.toVelocyPack(nodes);

  nodes.add("projections", VPackValue(VPackValueType::Array));
  // TODO: support projections?
  nodes.close();

  // And close it:
  nodes.close();
}

/// @brief estimateCost
CostEstimate SingleRemoteOperationNode::estimateCost() const {
  CostEstimate estimate = _dependencies[0]->getCost();
  return estimate;
}
