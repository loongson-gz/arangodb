////////////////////////////////////////////////////////////////////////////////
//
/// @brief Infrastructure for ExecutionPlans
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
/// @author Copyright 2014, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "ExecutionNode.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/Ast.h"
#include "Aql/CalculationExecutor.h"
#include "Aql/ClusterNodes.h"
#include "Aql/CollectNode.h"
#include "Aql/Collection.h"
#include "Aql/EnumerateCollectionExecutor.h"
#include "Aql/EnumerateListExecutor.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Expression.h"
#include "Aql/FilterExecutor.h"
#include "Aql/Function.h"
#include "Aql/IResearchViewNode.h"
#include "Aql/IdExecutor.h"
#include "Aql/IndexNode.h"
#include "Aql/KShortestPathsNode.h"
#include "Aql/LimitExecutor.h"
#include "Aql/ModificationNodes.h"
#include "Aql/NoResultsExecutor.h"
#include "Aql/NodeFinder.h"
#include "Aql/Query.h"
#include "Aql/Range.h"
#include "Aql/RegisterPlan.h"
#include "Aql/ReturnExecutor.h"
#include "Aql/ShortestPathNode.h"
#include "Aql/SortCondition.h"
#include "Aql/SortNode.h"
#include "Aql/SubqueryEndExecutionNode.h"
#include "Aql/SubqueryExecutor.h"
#include "Aql/SubqueryStartExecutionNode.h"
#include "Aql/TraversalNode.h"
#include "Aql/WalkerWorker.h"
#include "Aql/MaterializeExecutor.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/system-compiler.h"
#include "Cluster/ServerState.h"
#include "Meta/static_assert_size.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Methods.h"
#include "Utils/OperationCursor.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;
using namespace materialize;

namespace {

/// @brief NodeType to string mapping
std::unordered_map<int, std::string const> const typeNames{
    {static_cast<int>(ExecutionNode::SINGLETON), "SingletonNode"},
    {static_cast<int>(ExecutionNode::ENUMERATE_COLLECTION),
     "EnumerateCollectionNode"},
    {static_cast<int>(ExecutionNode::ENUMERATE_LIST), "EnumerateListNode"},
    {static_cast<int>(ExecutionNode::INDEX), "IndexNode"},
    {static_cast<int>(ExecutionNode::LIMIT), "LimitNode"},
    {static_cast<int>(ExecutionNode::CALCULATION), "CalculationNode"},
    {static_cast<int>(ExecutionNode::SUBQUERY), "SubqueryNode"},
    {static_cast<int>(ExecutionNode::FILTER), "FilterNode"},
    {static_cast<int>(ExecutionNode::SORT), "SortNode"},
    {static_cast<int>(ExecutionNode::COLLECT), "CollectNode"},
    {static_cast<int>(ExecutionNode::RETURN), "ReturnNode"},
    {static_cast<int>(ExecutionNode::REMOVE), "RemoveNode"},
    {static_cast<int>(ExecutionNode::INSERT), "InsertNode"},
    {static_cast<int>(ExecutionNode::UPDATE), "UpdateNode"},
    {static_cast<int>(ExecutionNode::REPLACE), "ReplaceNode"},
    {static_cast<int>(ExecutionNode::REMOTE), "RemoteNode"},
    {static_cast<int>(ExecutionNode::SCATTER), "ScatterNode"},
    {static_cast<int>(ExecutionNode::DISTRIBUTE), "DistributeNode"},
    {static_cast<int>(ExecutionNode::GATHER), "GatherNode"},
    {static_cast<int>(ExecutionNode::NORESULTS), "NoResultsNode"},
    {static_cast<int>(ExecutionNode::UPSERT), "UpsertNode"},
    {static_cast<int>(ExecutionNode::TRAVERSAL), "TraversalNode"},
    {static_cast<int>(ExecutionNode::SHORTEST_PATH), "ShortestPathNode"},
    {static_cast<int>(ExecutionNode::K_SHORTEST_PATHS), "KShortestPathsNode"},
    {static_cast<int>(ExecutionNode::REMOTESINGLE),
     "SingleRemoteOperationNode"},
    {static_cast<int>(ExecutionNode::ENUMERATE_IRESEARCH_VIEW),
     "EnumerateViewNode"},
    {static_cast<int>(ExecutionNode::SUBQUERY_START), "SubqueryStartNode"},
    {static_cast<int>(ExecutionNode::SUBQUERY_END), "SubqueryEndNode"},
    {static_cast<int>(ExecutionNode::DISTRIBUTE_CONSUMER),
     "DistributeConsumer"},
    {static_cast<int>(ExecutionNode::MATERIALIZE),
     "MaterializeNode"}};
}  // namespace

/// @brief resolve nodeType to a string.
std::string const& ExecutionNode::getTypeString(NodeType type) {
  auto it = ::typeNames.find(static_cast<int>(type));

  if (it != ::typeNames.end()) {
    return (*it).second;
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                 "missing type in TypeNames");
}

/// @brief returns the type name of the node
std::string const& ExecutionNode::getTypeString() const {
  return getTypeString(getType());
}

void ExecutionNode::validateType(int type) {
  auto it = ::typeNames.find(static_cast<int>(type));

  if (it == ::typeNames.end()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED, "unknown TypeID");
  }
}

bool ExecutionNode::isInSubquery() const {
  auto const* current = this;
  while (current != nullptr && current->hasDependency()) {
    current = current->getFirstDependency();
  }
  TRI_ASSERT(current != nullptr);
  return current->id() != 1;
}

/// @brief add a dependency
void ExecutionNode::addDependency(ExecutionNode* ep) {
  TRI_ASSERT(ep != nullptr);
  _dependencies.emplace_back(ep);
  ep->_parents.emplace_back(this);
}

/// @brief add a parent
void ExecutionNode::addParent(ExecutionNode* ep) {
  TRI_ASSERT(ep != nullptr);
  ep->_dependencies.emplace_back(this);
  _parents.emplace_back(ep);
}

void ExecutionNode::getSortElements(SortElementVector& elements, ExecutionPlan* plan,
                                    arangodb::velocypack::Slice const& slice,
                                    char const* which) {
  VPackSlice elementsSlice = slice.get("elements");

  if (!elementsSlice.isArray()) {
    std::string error = std::string("unexpected value for ") +
                        std::string(which) + std::string(" elements");
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, error);
  }

  elements.reserve(elementsSlice.length());

  for (VPackSlice it : VPackArrayIterator(elementsSlice)) {
    bool ascending = it.get("ascending").getBoolean();
    Variable* v = Variable::varFromVPack(plan->getAst(), it, "inVariable");
    elements.emplace_back(v, ascending);
    // Is there an attribute path?
    VPackSlice path = it.get("path");
    if (path.isArray()) {
      // Get a list of strings out and add to the path:
      auto& element = elements.back();
      for (auto const& it2 : VPackArrayIterator(path)) {
        if (it2.isString()) {
          element.attributePath.push_back(it2.copyString());
        }
      }
    }
  }
}

ExecutionNode* ExecutionNode::fromVPackFactory(ExecutionPlan* plan, VPackSlice const& slice) {
  int nodeTypeID = slice.get("typeID").getNumericValue<int>();
  validateType(nodeTypeID);

  NodeType nodeType = static_cast<NodeType>(nodeTypeID);

  switch (nodeType) {
    case SINGLETON:
      return new SingletonNode(plan, slice);
    case ENUMERATE_COLLECTION:
      return new EnumerateCollectionNode(plan, slice);
    case ENUMERATE_LIST:
      return new EnumerateListNode(plan, slice);
    case FILTER:
      return new FilterNode(plan, slice);
    case LIMIT:
      return new LimitNode(plan, slice);
    case CALCULATION:
      return new CalculationNode(plan, slice);
    case SUBQUERY:
      return new SubqueryNode(plan, slice);
    case SORT: {
      SortElementVector elements;
      getSortElements(elements, plan, slice, "SortNode");
      return new SortNode(plan, slice, elements, slice.get("stable").getBoolean());
    }
    case COLLECT: {
      Variable* expressionVariable =
          Variable::varFromVPack(plan->getAst(), slice, "expressionVariable", true);
      Variable* outVariable =
          Variable::varFromVPack(plan->getAst(), slice, "outVariable", true);

      // keepVariables
      std::vector<Variable const*> keepVariables;
      VPackSlice keepVariablesSlice = slice.get("keepVariables");
      if (keepVariablesSlice.isArray()) {
        for (VPackSlice it : VPackArrayIterator(keepVariablesSlice)) {
          Variable const* variable =
              Variable::varFromVPack(plan->getAst(), it, "variable");
          keepVariables.emplace_back(variable);
        }
      }

      // groups
      VPackSlice groupsSlice = slice.get("groups");
      if (!groupsSlice.isArray()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                       "invalid \"groups\" definition");
      }

      std::vector<std::pair<Variable const*, Variable const*>> groupVariables;
      {
        groupVariables.reserve(groupsSlice.length());
        for (VPackSlice it : VPackArrayIterator(groupsSlice)) {
          Variable* outVar =
              Variable::varFromVPack(plan->getAst(), it, "outVariable");
          Variable* inVar =
              Variable::varFromVPack(plan->getAst(), it, "inVariable");

          groupVariables.emplace_back(std::make_pair(outVar, inVar));
        }
      }

      // aggregates
      VPackSlice aggregatesSlice = slice.get("aggregates");
      if (!aggregatesSlice.isArray()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                       "invalid \"aggregates\" definition");
      }

      std::vector<std::pair<Variable const*, std::pair<Variable const*, std::string>>> aggregateVariables;
      {
        aggregateVariables.reserve(aggregatesSlice.length());
        for (VPackSlice it : VPackArrayIterator(aggregatesSlice)) {
          Variable* outVar =
              Variable::varFromVPack(plan->getAst(), it, "outVariable");
          Variable* inVar =
              Variable::varFromVPack(plan->getAst(), it, "inVariable");

          std::string const type = it.get("type").copyString();
          aggregateVariables.emplace_back(
              std::make_pair(outVar, std::make_pair(inVar, type)));
        }
      }

      bool count = slice.get("count").getBoolean();
      bool isDistinctCommand = slice.get("isDistinctCommand").getBoolean();

      auto node = new CollectNode(plan, slice, expressionVariable, outVariable, keepVariables,
                                  plan->getAst()->variables()->variables(false), groupVariables,
                                  aggregateVariables, count, isDistinctCommand);

      // specialize the node if required
      bool specialized = slice.get("specialized").getBoolean();
      if (specialized) {
        node->specialized();
      }

      return node;
    }
    case INSERT:
      return new InsertNode(plan, slice);
    case REMOVE:
      return new RemoveNode(plan, slice);
    case UPDATE:
      return new UpdateNode(plan, slice);
    case REPLACE:
      return new ReplaceNode(plan, slice);
    case UPSERT:
      return new UpsertNode(plan, slice);
    case RETURN:
      return new ReturnNode(plan, slice);
    case NORESULTS:
      return new NoResultsNode(plan, slice);
    case INDEX:
      return new IndexNode(plan, slice);
    case REMOTE:
      return new RemoteNode(plan, slice);
    case GATHER: {
      SortElementVector elements;
      getSortElements(elements, plan, slice, "GatherNode");
      return new GatherNode(plan, slice, elements);
    }
    case SCATTER:
      return new ScatterNode(plan, slice);
    case DISTRIBUTE:
      return new DistributeNode(plan, slice);
    case TRAVERSAL:
      return new TraversalNode(plan, slice);
    case SHORTEST_PATH:
      return new ShortestPathNode(plan, slice);
    case K_SHORTEST_PATHS:
      return new KShortestPathsNode(plan, slice);
    case REMOTESINGLE:
      return new SingleRemoteOperationNode(plan, slice);
    case ENUMERATE_IRESEARCH_VIEW:
      return new iresearch::IResearchViewNode(*plan, slice);
    case SUBQUERY_START:
      return new SubqueryStartNode(plan, slice);
    case SUBQUERY_END:
      return new SubqueryEndNode(plan, slice);
    case DISTRIBUTE_CONSUMER:
      return new DistributeConsumerNode(plan, slice);
    case MATERIALIZE:
      return createMaterializeNode(plan, slice);
    default: {
      // should not reach this point
      TRI_ASSERT(false);
    }
  }
  return nullptr;
}

/// @brief create an ExecutionNode from VPackSlice
ExecutionNode::ExecutionNode(ExecutionPlan* plan, VPackSlice const& slice)
    : _id(slice.get("id").getNumericValue<size_t>()),
      _depth(slice.get("depth").getNumericValue<int>()),
      _varUsageValid(true),
      _plan(plan),
      _isInSplicedSubquery(false) {
  TRI_ASSERT(_registerPlan.get() == nullptr);
  _registerPlan.reset(new RegisterPlan());
  _registerPlan->clear();
  _registerPlan->depth = _depth;
  _registerPlan->totalNrRegs = slice.get("totalNrRegs").getNumericValue<unsigned int>();

  VPackSlice varInfoList = slice.get("varInfoList");
  if (!varInfoList.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "\"varInfoList\" attribute needs to be an array");
  }

  _registerPlan->varInfo.reserve(varInfoList.length());

  for (VPackSlice it : VPackArrayIterator(varInfoList)) {
    if (!it.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_NOT_IMPLEMENTED,
          "\"varInfoList\" item needs to be an object");
    }
    VariableId variableId = it.get("VariableId").getNumericValue<VariableId>();
    RegisterId registerId = it.get("RegisterId").getNumericValue<RegisterId>();
    unsigned int depth = it.get("depth").getNumericValue<unsigned int>();

    _registerPlan->varInfo.try_emplace(variableId, VarInfo(depth, registerId));
  }

  VPackSlice nrRegsList = slice.get("nrRegs");
  if (!nrRegsList.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "\"nrRegs\" attribute needs to be an array");
  }

  _registerPlan->nrRegs.reserve(nrRegsList.length());
  for (VPackSlice it : VPackArrayIterator(nrRegsList)) {
    _registerPlan->nrRegs.emplace_back(it.getNumericValue<RegisterId>());
  }

  VPackSlice nrRegsHereList = slice.get("nrRegsHere");
  if (!nrRegsHereList.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                   "\"nrRegsHere\" needs to be an array");
  }

  _registerPlan->nrRegsHere.reserve(nrRegsHereList.length());
  for (VPackSlice it : VPackArrayIterator(nrRegsHereList)) {
    _registerPlan->nrRegsHere.emplace_back(it.getNumericValue<RegisterId>());
  }

  VPackSlice regsToClearList = slice.get("regsToClear");
  if (!regsToClearList.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                   "\"regsToClear\" needs to be an array");
  }

  _regsToClear.reserve(regsToClearList.length());
  for (VPackSlice it : VPackArrayIterator(regsToClearList)) {
    _regsToClear.insert(it.getNumericValue<RegisterId>());
  }

  auto allVars = plan->getAst()->variables();

  VPackSlice varsUsedLater = slice.get("varsUsedLater");
  if (!varsUsedLater.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                   "\"varsUsedLater\" needs to be an array");
  }

  _varsUsedLater.reserve(varsUsedLater.length());
  for (VPackSlice it : VPackArrayIterator(varsUsedLater)) {
    Variable oneVarUsedLater(it);
    Variable* oneVariable = allVars->getVariable(oneVarUsedLater.id);

    if (oneVariable == nullptr) {
      std::string errmsg = "varsUsedLater: ID not found in all-array: " +
                           StringUtils::itoa(oneVarUsedLater.id);
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED, errmsg);
    }
    _varsUsedLater.insert(oneVariable);
  }

  VPackSlice varsValidList = slice.get("varsValid");

  if (!varsValidList.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                   "\"varsValid\" needs to be an array");
  }

  _varsValid.reserve(varsValidList.length());
  for (VPackSlice it : VPackArrayIterator(varsValidList)) {
    Variable oneVarValid(it);
    Variable* oneVariable = allVars->getVariable(oneVarValid.id);

    if (oneVariable == nullptr) {
      std::string errmsg = "varsValid: ID not found in all-array: " +
                           StringUtils::itoa(oneVarValid.id);
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED, errmsg);
    }
    _varsValid.insert(oneVariable);
  }

  _isInSplicedSubquery = VelocyPackHelper::getBooleanValue(slice, "isInSplicedSubquery", false);
}

/// @brief toVelocyPack, export an ExecutionNode to VelocyPack
void ExecutionNode::toVelocyPack(VPackBuilder& builder, unsigned flags,
                                 bool keepTopLevelOpen) const {
  // default value is to NOT keep top level open
  builder.openObject();
  builder.add(VPackValue("nodes"));
  {
    std::unordered_set<ExecutionNode const*> seen;
    VPackArrayBuilder guard(&builder);
    toVelocyPackHelper(builder, flags, seen);
  }
  if (!keepTopLevelOpen) {
    builder.close();
  }
}

/// @brief execution Node clone utility to be called by derived classes
/// @return pointer to a registered node owned by a plan
ExecutionNode* ExecutionNode::cloneHelper(std::unique_ptr<ExecutionNode> other,
                                          bool withDependencies, bool withProperties) const {
  ExecutionPlan* plan = other->plan();

  if (plan == _plan) {
    // same execution plan for source and target
    // now assign a new id to the cloned node, otherwise it will fail
    // upon node registration and/or its meaning is ambiguous
    other->setId(plan->nextId());

    // cloning with properties will only work if we clone a node into
    // a different plan
    TRI_ASSERT(!withProperties);
  }

  other->_regsToClear = _regsToClear;
  other->_depth = _depth;
  other->_varUsageValid = _varUsageValid;
  other->_isInSplicedSubquery = _isInSplicedSubquery;

  if (withProperties) {
    auto allVars = plan->getAst()->variables();
    // Create new structures on the new AST...
    other->_varsUsedLater.reserve(_varsUsedLater.size());
    for (auto const& orgVar : _varsUsedLater) {
      auto var = allVars->getVariable(orgVar->id);
      TRI_ASSERT(var != nullptr);
      other->_varsUsedLater.insert(var);
    }

    other->_varsValid.reserve(_varsValid.size());

    for (auto const& orgVar : _varsValid) {
      auto var = allVars->getVariable(orgVar->id);
      TRI_ASSERT(var != nullptr);
      other->_varsValid.insert(var);
    }

    if (_registerPlan.get() != nullptr) {
      auto otherRegisterPlan =
          std::shared_ptr<RegisterPlan>(_registerPlan->clone(plan, _plan));
      other->_registerPlan = otherRegisterPlan;
    }
  } else {
    // point to current AST -> don't do deep copies.
    other->_varsUsedLater = _varsUsedLater;
    other->_varsValid = _varsValid;
    other->_registerPlan = _registerPlan;
  }

  auto* registeredNode = plan->registerNode(std::move(other));

  if (withDependencies) {
    cloneDependencies(plan, registeredNode, withProperties);
  }

  return registeredNode;
}

/// @brief helper for cloning, use virtual clone methods for dependencies
void ExecutionNode::cloneDependencies(ExecutionPlan* plan, ExecutionNode* theClone,
                                      bool withProperties) const {
  TRI_ASSERT(theClone != nullptr);
  auto it = _dependencies.begin();
  while (it != _dependencies.end()) {
    auto c = (*it)->clone(plan, true, withProperties);
    TRI_ASSERT(c != nullptr);
    try {
      c->_parents.emplace_back(theClone);
      theClone->_dependencies.emplace_back(c);
    } catch (...) {
      delete c;
      throw;
    }
    ++it;
  }
}

bool ExecutionNode::isEqualTo(ExecutionNode const& other) const {
  std::function<bool(ExecutionNode* const, ExecutionNode* const)> comparator =
      [](ExecutionNode* const l, ExecutionNode* const r) {
        return l->isEqualTo(*r);
      };

  return ((this->getType() == other.getType()) && (_id == other._id) &&
          (_depth == other._depth) &&
          (isInSplicedSubquery() == other.isInSplicedSubquery()) &&
          (std::equal(_dependencies.begin(), _dependencies.end(),
                      other._dependencies.begin(), comparator)));
}

/// @brief invalidate the cost estimation for the node and its dependencies
void ExecutionNode::invalidateCost() {
  _costEstimate.invalidate();

  for (auto& dep : _dependencies) {
    dep->invalidateCost();
  }
}

/// @brief estimate the cost of the node . . .
/// does not recalculate the estimate if already calculated
CostEstimate ExecutionNode::getCost() const {
  if (!_costEstimate.isValid()) {
    _costEstimate = estimateCost();
  }
  TRI_ASSERT(_costEstimate.estimatedCost >= 0.0);
  TRI_ASSERT(_costEstimate.isValid());
  return _costEstimate;
}

/// @brief functionality to walk an execution plan recursively
bool ExecutionNode::walk(WalkerWorker<ExecutionNode>& worker) {
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  // Only do every node exactly once
  // note: this check is not required normally because execution
  // plans do not contain cycles
  if (worker.done(this)) {
    return false;
  }
#endif

  if (worker.before(this)) {
    return true;
  }

  // Now the children in their natural order:
  for (auto const& it : _dependencies) {
    if (it->walk(worker)) {
      return true;
    }
  }

  // Now handle a subquery:
  if (getType() == SUBQUERY) {
    auto p = ExecutionNode::castTo<SubqueryNode*>(this);
    auto subquery = p->getSubquery();

    if (worker.enterSubquery(this, subquery)) {
      bool shouldAbort = subquery->walk(worker);
      worker.leaveSubquery(this, subquery);

      if (shouldAbort) {
        return true;
      }
    }
  }

  worker.after(this);

  return false;
}

/// @brief functionality to walk an execution plan recursively.
/// This variant of walk(), when visiting a node,
///  - first, when at a SubqueryNode, recurses into its subquery
///  - after that recurses on its dependencies.
/// This is in contrast to walk(), which recurses on the dependencies before
/// recursing into the subquery.
bool ExecutionNode::walkSubqueriesFirst(WalkerWorker<ExecutionNode>& worker) {
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  // Only do every node exactly once
  // note: this check is not required normally because execution
  // plans do not contain cycles
  if (worker.done(this)) {
    return false;
  }
#endif

  if (worker.before(this)) {
    return true;
  }

  // Now handle a subquery:
  if (getType() == SUBQUERY) {
    auto p = ExecutionNode::castTo<SubqueryNode*>(this);
    auto subquery = p->getSubquery();

    if (worker.enterSubquery(this, subquery)) {
      bool shouldAbort = subquery->walkSubqueriesFirst(worker);
      worker.leaveSubquery(this, subquery);

      if (shouldAbort) {
        return true;
      }
    }
  }

  // Now the children in their natural order:
  for (auto const& it : _dependencies) {
    if (it->walkSubqueriesFirst(worker)) {
      return true;
    }
  }

  worker.after(this);

  return false;
}

/// @brief get the surrounding loop
ExecutionNode const* ExecutionNode::getLoop() const {
  auto node = this;
  while (node != nullptr) {
    if (!node->hasDependency()) {
      return nullptr;
    }

    node = node->getFirstDependency();
    TRI_ASSERT(node != nullptr);

    auto type = node->getType();

    if (type == ENUMERATE_COLLECTION || type == INDEX || type == TRAVERSAL ||
        type == ENUMERATE_LIST || type == SHORTEST_PATH ||
        type == K_SHORTEST_PATHS || type == ENUMERATE_IRESEARCH_VIEW) {
      return node;
    }
  }

  return nullptr;
}

/// @brief toVelocyPackHelper, for a generic node
/// Note: The input nodes has to be an Array Element that is still Open.
///       At the end of this function the current-nodes Object is OPEN and
///       has to be closed. The initial caller of toVelocyPackHelper
///       has to close the array.
void ExecutionNode::toVelocyPackHelperGeneric(VPackBuilder& nodes, unsigned flags,
                                              std::unordered_set<ExecutionNode const*>& seen) const {
  TRI_ASSERT(nodes.isOpenArray());
  // We are not allowed to call if this node is already seen.
  TRI_ASSERT(seen.find(this) == seen.end());
  size_t const n = _dependencies.size();
  for (size_t i = 0; i < n; i++) {
    ExecutionNode const* dep = _dependencies[i];
    if (seen.find(dep) == seen.end()) {
      // Only toVelocypack those that have not been seen
      dep->toVelocyPackHelper(nodes, flags, seen);
    }
    // every dependency needs to be in this list!
    TRI_ASSERT(seen.find(dep) != seen.end());
  }
  // If this assert triggers we have created a loop.
  // There is a dependency path that leads back to this node
  TRI_ASSERT(seen.find(this) == seen.end());
  seen.emplace(this);
  nodes.openObject();
  nodes.add("type", VPackValue(getTypeString()));
  if (flags & ExecutionNode::SERIALIZE_DETAILS) {
    nodes.add("typeID", VPackValue(static_cast<int>(getType())));
  }
  nodes.add(VPackValue("dependencies"));  // Open Key
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& it : _dependencies) {
      nodes.add(VPackValue(it->id()));
    }
  }
  nodes.add("id", VPackValue(id()));
  if (flags & ExecutionNode::SERIALIZE_PARENTS) {
    nodes.add(VPackValue("parents"));  // Open Key
    VPackArrayBuilder guard(&nodes);
    for (auto const& it : _parents) {
      nodes.add(VPackValue(it->id()));
    }
  }
  if (flags & ExecutionNode::SERIALIZE_ESTIMATES) {
    CostEstimate estimate = getCost();
    nodes.add("estimatedCost", VPackValue(estimate.estimatedCost));
    nodes.add("estimatedNrItems", VPackValue(estimate.estimatedNrItems));
  }

  if (flags & ExecutionNode::SERIALIZE_DETAILS) {
    nodes.add("depth", VPackValue(_depth));

    if (_registerPlan) {
      nodes.add(VPackValue("varInfoList"));
      {
        VPackArrayBuilder guard(&nodes);
        for (auto const& oneVarInfo : _registerPlan->varInfo) {
          VPackObjectBuilder guardInner(&nodes);
          nodes.add("VariableId", VPackValue(oneVarInfo.first));
          nodes.add("depth", VPackValue(oneVarInfo.second.depth));
          nodes.add("RegisterId", VPackValue(oneVarInfo.second.registerId));
        }
      }
      nodes.add(VPackValue("nrRegs"));
      {
        VPackArrayBuilder guard(&nodes);
        for (auto const& oneRegisterID : _registerPlan->nrRegs) {
          nodes.add(VPackValue(oneRegisterID));
        }
      }
      nodes.add(VPackValue("nrRegsHere"));
      {
        VPackArrayBuilder guard(&nodes);
        for (auto const& oneRegisterID : _registerPlan->nrRegsHere) {
          nodes.add(VPackValue(oneRegisterID));
        }
      }
      nodes.add("totalNrRegs", VPackValue(_registerPlan->totalNrRegs));
    } else {
      nodes.add(VPackValue("varInfoList"));
      { VPackArrayBuilder guard(&nodes); }
      nodes.add(VPackValue("nrRegs"));
      { VPackArrayBuilder guard(&nodes); }
      nodes.add(VPackValue("nrRegsHere"));
      { VPackArrayBuilder guard(&nodes); }
      nodes.add("totalNrRegs", VPackValue(0));
    }

    nodes.add(VPackValue("regsToClear"));
    {
      VPackArrayBuilder guard(&nodes);
      for (auto const& oneRegisterID : _regsToClear) {
        nodes.add(VPackValue(oneRegisterID));
      }
    }

    nodes.add(VPackValue("varsUsedLater"));
    {
      VPackArrayBuilder guard(&nodes);
      for (auto const& oneVar : _varsUsedLater) {
        oneVar->toVelocyPack(nodes);
      }
    }

    nodes.add(VPackValue("varsValid"));
    {
      VPackArrayBuilder guard(&nodes);
      for (auto const& oneVar : _varsValid) {
        oneVar->toVelocyPack(nodes);
      }
    }

    nodes.add("isInSplicedSubquery", VPackValue(_isInSplicedSubquery));
  }
  TRI_ASSERT(nodes.isOpenObject());
}

/// @brief static analysis debugger
#if 0
struct RegisterPlanningDebugger final : public WalkerWorker<ExecutionNode> {
  RegisterPlanningDebugger()
    : indent(0) {
  }

  ~RegisterPlanningDebugger () = default;

  int indent;

  bool enterSubquery(ExecutionNode*, ExecutionNode*) override final {
    indent++;
    return true;
  }

  void leaveSubquery(ExecutionNode*, ExecutionNode*) override final {
    indent--;
  }

  void after(ExecutionNode* ep) override final {
    for (int i = 0; i < indent; i++) {
      std::cout << " ";
    }
    std::cout << ep->getTypeString() << " ";
    std::cout << "regsUsedHere: ";
    ::arangodb::containers::HashSet<Variable const*> variablesUsedHere;
    ep->getVariablesUsedHere(variablesUsedHere);
    for (auto const& v : variablesUsedHere) {
      std::cout << ep->getRegisterPlan()->varInfo.find(v->id)->second.registerId
                << " ";
    }
    std::cout << "regsSetHere: ";
    for (auto const& v : ep->getVariablesSetHere()) {
      std::cout << ep->getRegisterPlan()->varInfo.find(v->id)->second.registerId
                << " ";
    }
    std::cout << "regsToClear: ";
    for (auto const& r : ep->getRegsToClear()) {
      std::cout << r << " ";
    }
    std::cout << std::endl;
  }
};

#endif

/// @brief planRegisters
void ExecutionNode::planRegisters(ExecutionNode* super) {
  // The super is only for the case of subqueries.
  std::shared_ptr<RegisterPlan> v;

  if (super == nullptr) {
    v.reset(new RegisterPlan());
  } else {
    v.reset(new RegisterPlan(*(super->_registerPlan), super->_depth));
  }
  v->setSharedPtr(&v);

  walk(*v);
  // Now handle the subqueries:
  for (auto& s : v->subQueryNodes) {
    if (s->getType() == ExecutionNode::NodeType::SUBQUERY) {
      auto sq = ExecutionNode::castTo<SubqueryNode*>(s);
      sq->getSubquery()->planRegisters(s);
    }
  }
  v->reset();

  // Just for debugging:
  /*
  std::cout << std::endl;
  RegisterPlanningDebugger debugger;
  walk(debugger);
  std::cout << std::endl;
  */
}

RegisterId ExecutionNode::varToRegUnchecked(Variable const& var) const {
  std::unordered_map<VariableId, VarInfo> const& varInfo = getRegisterPlan()->varInfo;
  auto const it = varInfo.find(var.id);
  TRI_ASSERT(it != varInfo.end());
  RegisterId const reg = it->second.registerId;

  return reg;
}


bool ExecutionNode::isInSplicedSubquery() const noexcept {
  return _isInSplicedSubquery;
}

void ExecutionNode::setIsInSplicedSubquery(bool const value) noexcept {
  _isInSplicedSubquery = value;
}

/// @brief replace a dependency, returns true if the pointer was found and
/// replaced, please note that this does not delete oldNode!
bool ExecutionNode::replaceDependency(ExecutionNode* oldNode, ExecutionNode* newNode) {
  TRI_ASSERT(oldNode != nullptr);
  TRI_ASSERT(newNode != nullptr);

  auto it = _dependencies.begin();

  while (it != _dependencies.end()) {
    if (*it == oldNode) {
      *it = newNode;
      try {
        newNode->_parents.emplace_back(this);
      } catch (...) {
        *it = oldNode;  // roll back
        return false;
      }
      try {
        for (auto it2 = oldNode->_parents.begin(); it2 != oldNode->_parents.end(); ++it2) {
          if (*it2 == this) {
            oldNode->_parents.erase(it2);
            break;
          }
        }
      } catch (...) {
        // If this happens, we ignore that the _parents of oldNode
        // are not set correctly
      }
      return true;
    }

    ++it;
  }
  return false;
}

/// @brief remove a dependency, returns true if the pointer was found and
/// removed, please note that this does not delete ep!
bool ExecutionNode::removeDependency(ExecutionNode* ep) {
  bool ok = false;
  for (auto it = _dependencies.begin(); it != _dependencies.end(); ++it) {
    if (*it == ep) {
      try {
        it = _dependencies.erase(it);
      } catch (...) {
        return false;
      }
      ok = true;
      break;
    }
  }

  if (!ok) {
    return false;
  }

  // Now remove us as a parent of the old dependency as well:
  for (auto it = ep->_parents.begin(); it != ep->_parents.end(); ++it) {
    if (*it == this) {
      try {
        ep->_parents.erase(it);
      } catch (...) {
      }
      return true;
    }
  }

  return false;
}

/// @brief remove all dependencies for the given node
void ExecutionNode::removeDependencies() {
  for (auto& x : _dependencies) {
    for (auto it = x->_parents.begin(); it != x->_parents.end();
         /* no hoisting */) {
      if (*it == this) {
        try {
          it = x->_parents.erase(it);
        } catch (...) {
        }
        break;
      } else {
        ++it;
      }
    }
  }
  _dependencies.clear();
}

std::unordered_set<RegisterId> ExecutionNode::calcRegsToKeep() const {
  ExecutionNode const* const previousNode = getFirstDependency();
  // Only the Singleton has no previousNode, and it does not call this method.
  TRI_ASSERT(previousNode != nullptr);

  bool inserted = false;
  RegisterId const nrInRegs = getRegisterPlan()->nrRegs[previousNode->getDepth()];
  std::unordered_set<RegisterId> regsToKeep{};
  regsToKeep.reserve(getVarsUsedLater().size());

  for (auto const var : getVarsUsedLater()) {
    auto reg = variableToRegisterId(var);
    if (reg < nrInRegs) {
      std::tie(std::ignore, inserted) = regsToKeep.emplace(reg);
      TRI_ASSERT(inserted);
    }
  }

  return regsToKeep;
}

RegisterId ExecutionNode::variableToRegisterId(Variable const* variable) const {
  TRI_ASSERT(variable != nullptr);
  auto it = getRegisterPlan()->varInfo.find(variable->id);
  TRI_ASSERT(it != getRegisterPlan()->varInfo.end());
  RegisterId rv = it->second.registerId;
  TRI_ASSERT(rv < RegisterPlan::MaxRegisterId);
  return rv;
}

// This is the general case and will not work if e.g. there is no predecessor.
ExecutorInfos ExecutionNode::createRegisterInfos(
    std::shared_ptr<std::unordered_set<RegisterId>>&& readableInputRegisters,
    std::shared_ptr<std::unordered_set<RegisterId>>&& writableOutputRegisters) const {
  RegisterId const nrOutRegs = getNrOutputRegisters();
  RegisterId const nrInRegs = getNrInputRegisters();

  std::unordered_set<RegisterId> regsToKeep = calcRegsToKeep();
  std::unordered_set<RegisterId> regsToClear = getRegsToClear();

  return ExecutorInfos{std::move(readableInputRegisters),
                       std::move(writableOutputRegisters),
                       nrInRegs,
                       nrOutRegs,
                       std::move(regsToClear),
                       std::move(regsToKeep)};
}

RegisterId ExecutionNode::getNrInputRegisters() const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);
  return getRegisterPlan()->nrRegs[previousNode->getDepth()];
}

RegisterId ExecutionNode::getNrOutputRegisters() const {
  return getRegisterPlan()->nrRegs[getDepth()];
}

ExecutionNode::ExecutionNode(ExecutionPlan* plan, size_t id)
    : _id(id), _depth(0), _varUsageValid(false), _plan(plan), _isInSplicedSubquery(false) {}

ExecutionNode::~ExecutionNode() = default;

size_t ExecutionNode::id() const { return _id; }

void ExecutionNode::swapFirstDependency(ExecutionNode* node) {
  TRI_ASSERT(hasDependency());
  _dependencies[0] = node;
}

std::vector<ExecutionNode*> const& ExecutionNode::getDependencies() const {
  return _dependencies;
}

ExecutionNode* ExecutionNode::getFirstDependency() const {
  if (_dependencies.empty()) {
    return nullptr;
  }
  TRI_ASSERT(_dependencies[0] != nullptr);
  return _dependencies[0];
}

bool ExecutionNode::hasDependency() const {
  return (_dependencies.size() == 1);
}

void ExecutionNode::dependencies(std::vector<ExecutionNode*>& result) const {
  for (auto const& it : _dependencies) {
    TRI_ASSERT(it != nullptr);
    result.emplace_back(it);
  }
}

std::vector<ExecutionNode*> ExecutionNode::getParents() const {
  return _parents;
}

bool ExecutionNode::hasParent() const { return (_parents.size() == 1); }
  
/// @brief whether or not the node has any ancestor (parent at any distance)
/// of this type
bool ExecutionNode::hasParentOfType(ExecutionNode::NodeType type) const {
  ExecutionNode* current = getFirstParent();
  while (current != nullptr) {
    if (current->getType() == type) {
      return true;
    }
    current = current->getFirstParent();
  }
  return false;
}

ExecutionNode* ExecutionNode::getFirstParent() const {
  if (_parents.empty()) {
    return nullptr;
  }
  TRI_ASSERT(_parents[0] != nullptr);
  return _parents[0];
}

void ExecutionNode::parents(std::vector<ExecutionNode*>& result) const {
  for (auto const& it : _parents) {
    TRI_ASSERT(it != nullptr);
    result.emplace_back(it);
  }
}

ExecutionNode const* ExecutionNode::getSingleton() const {
  auto node = this;
  do {
    node = node->getFirstDependency();
  } while (node != nullptr && node->getType() != SINGLETON);

  return node;
}

ExecutionNode* ExecutionNode::getSingleton() {
  auto node = this;
  do {
    node = node->getFirstDependency();
  } while (node != nullptr && node->getType() != SINGLETON);

  return node;
}

void ExecutionNode::getDependencyChain(std::vector<ExecutionNode*>& result, bool includeSelf) {
  auto current = this;
  while (current != nullptr) {
    if (includeSelf || current != this) {
      result.emplace_back(current);
    }
    current = current->getFirstDependency();
  }
}

void ExecutionNode::setParent(ExecutionNode* p) {
  _parents.clear();
  _parents.emplace_back(p);
}

void ExecutionNode::getVariablesUsedHere(::arangodb::containers::HashSet<const Variable*>&) const {
  // do nothing!
}

std::vector<Variable const*> ExecutionNode::getVariablesSetHere() const {
  return std::vector<Variable const*>();
}

::arangodb::containers::HashSet<VariableId> ExecutionNode::getVariableIdsUsedHere() const {
  ::arangodb::containers::HashSet<Variable const*> vars;
  getVariablesUsedHere(vars);

  ::arangodb::containers::HashSet<VariableId> ids;
  for (auto& it : vars) {
    ids.emplace(it->id);
  }
  return ids;
}

bool ExecutionNode::setsVariable(const ::arangodb::containers::HashSet<const Variable*>& which) const {
  for (auto const& v : getVariablesSetHere()) {
    if (which.find(v) != which.end()) {
      return true;
    }
  }
  return false;
}

void ExecutionNode::setVarsUsedLater(const ::arangodb::containers::HashSet<const Variable*>& v) {
  _varsUsedLater = v;
}

const ::arangodb::containers::HashSet<const Variable*>& ExecutionNode::getVarsUsedLater() const {
  TRI_ASSERT(_varUsageValid);
  return _varsUsedLater;
}

void ExecutionNode::setVarsValid(::arangodb::containers::HashSet<const Variable*>& v) {
  _varsValid = v;
}

const ::arangodb::containers::HashSet<const Variable*>& ExecutionNode::getVarsValid() const {
  TRI_ASSERT(_varUsageValid);
  return _varsValid;
}

void ExecutionNode::setVarUsageValid() { _varUsageValid = true; }

void ExecutionNode::invalidateVarUsage() {
  _varsUsedLater.clear();
  _varsValid.clear();
  _varUsageValid = false;
}

bool ExecutionNode::isDeterministic() { return true; }

bool ExecutionNode::isModificationNode() const {
  // derived classes can change this
  return false;
}

ExecutionPlan const* ExecutionNode::plan() const { return _plan; }

ExecutionPlan* ExecutionNode::plan() { return _plan; }

std::shared_ptr<RegisterPlan> ExecutionNode::getRegisterPlan() const {
  TRI_ASSERT(_registerPlan != nullptr);
  return _registerPlan;
}

int ExecutionNode::getDepth() const { return _depth; }

std::unordered_set<RegisterId> const& ExecutionNode::getRegsToClear() const {
  return _regsToClear;
}

bool ExecutionNode::isVarUsedLater(Variable const* variable) const {
  return (_varsUsedLater.find(variable) != _varsUsedLater.end());
}

bool ExecutionNode::isInInnerLoop() const { return getLoop() != nullptr; }

void ExecutionNode::setId(size_t id) { _id = id; }

void ExecutionNode::setRegsToClear(std::unordered_set<RegisterId>&& toClear) {
  _regsToClear = std::move(toClear);
}

RegisterId ExecutionNode::variableToRegisterOptionalId(Variable const* var) const {
  if (var) {
    return variableToRegisterId(var);
  }
  return RegisterPlan::MaxRegisterId;
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> SingletonNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  // number in == number out
  // Other nodes get the nrInRegs from the previous node.
  // That is why we do not use `calcRegsToKeep()`
  RegisterId const nrRegs = getRegisterPlan()->nrRegs[getDepth()];

  std::unordered_set<RegisterId> toKeep;
  if (isInSubquery()) {
    for (auto const& var : this->getVarsUsedLater()) {
      auto val = variableToRegisterId(var);
      if (val < nrRegs) {
        auto rv = toKeep.insert(val);
        TRI_ASSERT(rv.second);
      }
    }
  }

  IdExecutorInfos infos(nrRegs, std::move(toKeep), getRegsToClear());

  return std::make_unique<ExecutionBlockImpl<IdExecutor<BlockPassthrough::Enable, ConstFetcher>>>(
      &engine, this, std::move(infos));
}

/// @brief toVelocyPack, for SingletonNode
void SingletonNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                       std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);
  // This node has no own information.
  nodes.close();
}

/// @brief the cost of a singleton is 1, it produces one item only
CostEstimate SingletonNode::estimateCost() const {
  CostEstimate estimate = CostEstimate::empty();
  estimate.estimatedNrItems = 1;
  estimate.estimatedCost = 1.0;
  return estimate;
}

SingletonNode::SingletonNode(ExecutionPlan* plan, size_t id)
    : ExecutionNode(plan, id) {}

SingletonNode::SingletonNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base) {}

ExecutionNode::NodeType SingletonNode::getType() const { return SINGLETON; }

EnumerateCollectionNode::EnumerateCollectionNode(ExecutionPlan* plan,
                                                 arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      DocumentProducingNode(plan, base),
      CollectionAccessingNode(plan, base),
      _random(base.get("random").getBoolean()),
      _hint(base) {}

/// @brief toVelocyPack, for EnumerateCollectionNode
void EnumerateCollectionNode::toVelocyPackHelper(VPackBuilder& builder, unsigned flags,
                                                 std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(builder, flags, seen);

  builder.add("random", VPackValue(_random));

  _hint.toVelocyPack(builder);

  // add outvariable and projection
  DocumentProducingNode::toVelocyPack(builder, flags);

  // add collection information
  CollectionAccessingNode::toVelocyPack(builder, flags);

  // And close it:
  builder.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> EnumerateCollectionNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  EnumerateCollectionExecutorInfos infos(
      variableToRegisterId(_outVariable),
      getRegisterPlan()->nrRegs[previousNode->getDepth()],
      getRegisterPlan()->nrRegs[getDepth()], getRegsToClear(), calcRegsToKeep(),
      &engine, this->_collection, _outVariable, (this->isVarUsedLater(_outVariable) || this->_filter != nullptr),
      this->_filter.get(),
      this->projections(), this->coveringIndexAttributePositions(),
      EngineSelectorFeature::ENGINE->useRawDocumentPointers(), this->_random);
  return std::make_unique<ExecutionBlockImpl<EnumerateCollectionExecutor>>(&engine, this,
                                                                           std::move(infos));
}

/// @brief clone ExecutionNode recursively
ExecutionNode* EnumerateCollectionNode::clone(ExecutionPlan* plan, bool withDependencies,
                                              bool withProperties) const {
  auto outVariable = _outVariable;
  if (withProperties) {
    outVariable = plan->getAst()->variables()->createVariable(outVariable);
    TRI_ASSERT(outVariable != nullptr);
  }

  auto c = std::make_unique<EnumerateCollectionNode>(plan, _id, _collection,
                                                     outVariable, _random, _hint);

  c->projections(_projections);
  CollectionAccessingNode::cloneInto(*c);
  DocumentProducingNode::cloneInto(plan, *c);

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

void EnumerateCollectionNode::setRandom() { _random = true; }

bool EnumerateCollectionNode::isDeterministic() { return !_random; }

std::vector<Variable const*> EnumerateCollectionNode::getVariablesSetHere() const {
  return std::vector<Variable const*>{_outVariable};
}

/// @brief the cost of an enumerate collection node is a multiple of the cost of
/// its unique dependency
CostEstimate EnumerateCollectionNode::estimateCost() const {
  transaction::Methods* trx = _plan->getAst()->query()->trx();
  if (trx->status() != transaction::Status::RUNNING) {
    return CostEstimate::empty();
  }

  TRI_ASSERT(!_dependencies.empty());
  CostEstimate estimate = _dependencies.at(0)->getCost();
  estimate.estimatedNrItems *= _collection->count(trx);
  // We do a full collection scan for each incoming item.
  // random iteration is slightly more expensive than linear iteration
  // we also penalize each EnumerateCollectionNode slightly (and do not
  // do the same for IndexNodes) so IndexNodes will be preferred
  estimate.estimatedCost += estimate.estimatedNrItems * (_random ? 1.005 : 1.0) + 1.0;
  return estimate;
}
  
EnumerateListNode::EnumerateListNode(ExecutionPlan* plan,
                                     arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _inVariable(Variable::varFromVPack(plan->getAst(), base, "inVariable")),
      _outVariable(Variable::varFromVPack(plan->getAst(), base, "outVariable")) {}

/// @brief toVelocyPack, for EnumerateListNode
void EnumerateListNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                           std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);
  nodes.add(VPackValue("inVariable"));
  _inVariable->toVelocyPack(nodes);

  nodes.add(VPackValue("outVariable"));
  _outVariable->toVelocyPack(nodes);

  // And close it:
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> EnumerateListNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);
  RegisterId inputRegister = variableToRegisterId(_inVariable);
  RegisterId outRegister = variableToRegisterId(_outVariable);
  EnumerateListExecutorInfos infos(inputRegister, outRegister,
                                   getRegisterPlan()->nrRegs[previousNode->getDepth()],
                                   getRegisterPlan()->nrRegs[getDepth()],
                                   getRegsToClear(), calcRegsToKeep());
  return std::make_unique<ExecutionBlockImpl<EnumerateListExecutor>>(&engine, this,
                                                                     std::move(infos));
}

/// @brief clone ExecutionNode recursively
ExecutionNode* EnumerateListNode::clone(ExecutionPlan* plan, bool withDependencies,
                                        bool withProperties) const {
  auto outVariable = _outVariable;
  auto inVariable = _inVariable;

  if (withProperties) {
    outVariable = plan->getAst()->variables()->createVariable(outVariable);
    inVariable = plan->getAst()->variables()->createVariable(inVariable);
  }

  auto c = std::make_unique<EnumerateListNode>(plan, _id, inVariable, outVariable);

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

/// @brief the cost of an enumerate list node
CostEstimate EnumerateListNode::estimateCost() const {
  // Well, what can we say? The length of the list can in general
  // only be determined at runtime... If we were to know that this
  // list is constant, then we could maybe multiply by the length
  // here... For the time being, we assume 100
  size_t length = 100;

  auto setter = _plan->getVarSetBy(_inVariable->id);

  if (setter != nullptr) {
    if (setter->getType() == ExecutionNode::CALCULATION) {
      // list variable introduced by a calculation
      auto expression = ExecutionNode::castTo<CalculationNode*>(setter)->expression();

      if (expression != nullptr) {
        auto node = expression->node();

        if (node->type == NODE_TYPE_ARRAY) {
          // this one is easy
          length = node->numMembers();
        }
        if (node->type == NODE_TYPE_RANGE) {
          auto low = node->getMember(0);
          auto high = node->getMember(1);

          if (low->isConstant() && high->isConstant() &&
              (low->isValueType(VALUE_TYPE_INT) || low->isValueType(VALUE_TYPE_DOUBLE)) &&
              (high->isValueType(VALUE_TYPE_INT) || high->isValueType(VALUE_TYPE_DOUBLE))) {
            // create a temporary range to determine the size
            Range range(low->getIntValue(), high->getIntValue());

            length = range.size();
          }
        }
      }
    } else if (setter->getType() == ExecutionNode::SUBQUERY) {
      // length will be set by the subquery's cost estimator
      CostEstimate subEstimate =
          ExecutionNode::castTo<SubqueryNode const*>(setter)->getSubquery()->getCost();
      length = subEstimate.estimatedNrItems;
    }
  }

  TRI_ASSERT(!_dependencies.empty());
  CostEstimate estimate = _dependencies.at(0)->getCost();
  estimate.estimatedNrItems *= length;
  estimate.estimatedCost += estimate.estimatedNrItems;
  return estimate;
}

EnumerateListNode::EnumerateListNode(ExecutionPlan* plan, size_t id,
                                     Variable const* inVariable, Variable const* outVariable)
    : ExecutionNode(plan, id), _inVariable(inVariable), _outVariable(outVariable) {
  TRI_ASSERT(_inVariable != nullptr);
  TRI_ASSERT(_outVariable != nullptr);
}

ExecutionNode::NodeType EnumerateListNode::getType() const {
  return ENUMERATE_LIST;
}

void EnumerateListNode::getVariablesUsedHere(::arangodb::containers::HashSet<const Variable*>& vars) const {
  vars.emplace(_inVariable);
}

std::vector<Variable const*> EnumerateListNode::getVariablesSetHere() const {
  return std::vector<Variable const*>{_outVariable};
}

Variable const* EnumerateListNode::inVariable() const { return _inVariable; }

Variable const* EnumerateListNode::outVariable() const { return _outVariable; }

LimitNode::LimitNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _offset(base.get("offset").getNumericValue<decltype(_offset)>()),
      _limit(base.get("limit").getNumericValue<decltype(_limit)>()),
      _fullCount(base.get("fullCount").getBoolean()) {}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> LimitNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  // Fullcount must only be enabled on the last limit node on the main level
  TRI_ASSERT(!_fullCount || !isInSubquery());

  LimitExecutorInfos infos(getRegisterPlan()->nrRegs[previousNode->getDepth()],
                           getRegisterPlan()->nrRegs[getDepth()], getRegsToClear(),
                           calcRegsToKeep(), _offset, _limit, _fullCount);
  return std::make_unique<ExecutionBlockImpl<LimitExecutor>>(&engine, this,
                                                             std::move(infos));
}

// @brief toVelocyPack, for LimitNode
void LimitNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                   std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);
  nodes.add("offset", VPackValue(_offset));
  nodes.add("limit", VPackValue(_limit));
  nodes.add("fullCount", VPackValue(_fullCount));

  // And close it:
  nodes.close();
}

/// @brief estimateCost
CostEstimate LimitNode::estimateCost() const {
  TRI_ASSERT(!_dependencies.empty());
  CostEstimate estimate = _dependencies.at(0)->getCost();
  estimate.estimatedNrItems =
      (std::min)(_limit, (std::max)(static_cast<size_t>(0),
                                    estimate.estimatedNrItems - _offset));
  estimate.estimatedCost += estimate.estimatedNrItems;
  return estimate;
}

LimitNode::LimitNode(ExecutionPlan* plan, size_t id, size_t offset, size_t limit)
    : ExecutionNode(plan, id), _offset(offset), _limit(limit), _fullCount(false) {}

ExecutionNode::NodeType LimitNode::getType() const { return LIMIT; }

ExecutionNode* LimitNode::clone(ExecutionPlan* plan, bool withDependencies,
                                bool withProperties) const {
  auto c = std::make_unique<LimitNode>(plan, _id, _offset, _limit);

  if (_fullCount) {
    c->setFullCount();
  }

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

void LimitNode::setFullCount() { _fullCount = true; }

bool LimitNode::fullCount() const noexcept { return _fullCount; }

size_t LimitNode::offset() const { return _offset; }

size_t LimitNode::limit() const { return _limit; }

CalculationNode::CalculationNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _outVariable(Variable::varFromVPack(plan->getAst(), base, "outVariable")),
      _expression(new Expression(plan, plan->getAst(), base)) {}

/// @brief toVelocyPack, for CalculationNode
void CalculationNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                         std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);
  nodes.add(VPackValue("expression"));
  _expression->toVelocyPack(nodes, flags);

  nodes.add(VPackValue("outVariable"));
  _outVariable->toVelocyPack(nodes);

  nodes.add("canThrow", VPackValue(false));

  nodes.add("expressionType", VPackValue(_expression->typeString()));

  if ((flags & SERIALIZE_FUNCTIONS) && _expression->node() != nullptr) {
    auto root = _expression->node();
    if (root != nullptr) {
      // enumerate all used functions, but report each function only once
      std::unordered_set<std::string> functionsSeen;
      nodes.add("functions", VPackValue(VPackValueType::Array));

      Ast::traverseReadOnly(root, [&functionsSeen, &nodes](AstNode const* node) -> bool {
        if (node->type == NODE_TYPE_FCALL) {
          auto func = static_cast<Function const*>(node->getData());
          if (functionsSeen.insert(func->name).second) {
            // built-in function, not seen before
            nodes.openObject();
            nodes.add("name", VPackValue(func->name));
            nodes.add("isDeterministic",
                      VPackValue(func->hasFlag(Function::Flags::Deterministic)));
            nodes.add("canRunOnDBServer",
                      VPackValue(func->hasFlag(Function::Flags::CanRunOnDBServer)));
            nodes.add("cacheable", VPackValue(func->hasFlag(Function::Flags::Cacheable)));
            nodes.add("usesV8", VPackValue(func->implementation == nullptr));
            nodes.close();
          }
        } else if (node->type == NODE_TYPE_FCALL_USER) {
          auto func = node->getString();
          if (functionsSeen.insert(func).second) {
            // user defined function, not seen before
            nodes.openObject();
            nodes.add("name", VPackValue(func));
            nodes.add("isDeterministic", VPackValue(false));
            nodes.add("canRunOnDBServer", VPackValue(false));
            nodes.add("usesV8", VPackValue(true));
            nodes.close();
          }
        }
        return true;
      });

      nodes.close();
    }
  }

  // And close it
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> CalculationNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId outputRegister = variableToRegisterId(_outVariable);

  ::arangodb::containers::HashSet<Variable const*> inVars;
  _expression->variables(inVars);

  std::vector<Variable const*> expInVars;
  expInVars.reserve(inVars.size());
  std::vector<RegisterId> expInRegs;
  expInRegs.reserve(inVars.size());

  for (auto& var : inVars) {
    expInVars.emplace_back(var);
    expInRegs.emplace_back(variableToRegisterId(var));
  }

  bool const isReference = (expression()->node()->type == NODE_TYPE_REFERENCE);
  if (isReference) {
    TRI_ASSERT(expInRegs.size() == 1);
  }
  bool const willUseV8 = expression()->willUseV8();

  TRI_ASSERT(engine.getQuery() != nullptr);
  TRI_ASSERT(expression() != nullptr);

  CalculationExecutorInfos infos(
      outputRegister, getRegisterPlan()->nrRegs[previousNode->getDepth()],
      getRegisterPlan()->nrRegs[getDepth()], getRegsToClear(), calcRegsToKeep(),
      *engine.getQuery() /* used for v8 contexts and in expression */,
      *expression(), std::move(expInVars) /* required by expression.execute */,
      std::move(expInRegs)); /* required by expression.execute */

  if (isReference) {
    return std::make_unique<ExecutionBlockImpl<CalculationExecutor<CalculationType::Reference>>>(
        &engine, this, std::move(infos));
  } else if (!willUseV8) {
    return std::make_unique<ExecutionBlockImpl<CalculationExecutor<CalculationType::Condition>>>(
        &engine, this, std::move(infos));
  } else {
    return std::make_unique<ExecutionBlockImpl<CalculationExecutor<CalculationType::V8Condition>>>(
        &engine, this, std::move(infos));
  }
}

ExecutionNode* CalculationNode::clone(ExecutionPlan* plan, bool withDependencies,
                                      bool withProperties) const {
  auto outVariable = _outVariable;

  if (withProperties) {
    outVariable = plan->getAst()->variables()->createVariable(outVariable);
  }

  auto c = std::make_unique<CalculationNode>(plan, _id,
                                             _expression->clone(plan, plan->getAst()),
                                             outVariable);

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

/// @brief estimateCost
CostEstimate CalculationNode::estimateCost() const {
  TRI_ASSERT(!_dependencies.empty());
  CostEstimate estimate = _dependencies.at(0)->getCost();
  estimate.estimatedCost += estimate.estimatedNrItems;
  return estimate;
}

CalculationNode::CalculationNode(ExecutionPlan* plan, size_t id,
                                 std::unique_ptr<Expression> expr, Variable const* outVariable)
    : ExecutionNode(plan, id),
      _outVariable(outVariable),
      _expression(std::move(expr)) {
  TRI_ASSERT(_expression != nullptr);
  TRI_ASSERT(_outVariable != nullptr);
}

CalculationNode::~CalculationNode() {}

ExecutionNode::NodeType CalculationNode::getType() const { return CALCULATION; }

Variable const* CalculationNode::outVariable() const { return _outVariable; }

Expression* CalculationNode::expression() const { return _expression.get(); }

void CalculationNode::getVariablesUsedHere(::arangodb::containers::HashSet<const Variable*>& vars) const {
  _expression->variables(vars);
}

std::vector<Variable const*> CalculationNode::getVariablesSetHere() const {
  return std::vector<Variable const*>{_outVariable};
}

bool CalculationNode::isDeterministic() {
  return _expression->isDeterministic();
}

SubqueryNode::SubqueryNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _subquery(nullptr),
      _outVariable(Variable::varFromVPack(plan->getAst(), base, "outVariable")) {}

/// @brief toVelocyPack, for SubqueryNode
void SubqueryNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                      std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);

  nodes.add(VPackValue("subquery"));
  _subquery->toVelocyPack(nodes, flags, /*keepTopLevelOpen*/ false);
  nodes.add(VPackValue("outVariable"));
  _outVariable->toVelocyPack(nodes);

  nodes.add("isConst", VPackValue(const_cast<SubqueryNode*>(this)->isConst()));

  // And add it:
  nodes.close();
}

/// @brief invalidate the cost estimation for the node and its dependencies
void SubqueryNode::invalidateCost() {
  ExecutionNode::invalidateCost();
  // pass invalidation call to subquery too
  getSubquery()->invalidateCost();
}

bool SubqueryNode::isConst() {
  if (isModificationSubquery() || !isDeterministic()) {
    return false;
  }

  if (mayAccessCollections() && _plan->getAst()->query()->isModificationQuery()) {
    // a subquery that accesses data from a collection may not be const,
    // even if itself does not modify any data. it is possible that the
    // subquery is embedded into some outer loop that is modifying data
    return false;
  }

  ::arangodb::containers::HashSet<Variable const*> vars;
  getVariablesUsedHere(vars);
  for (auto const& v : vars) {
    auto setter = _plan->getVarSetBy(v->id);

    if (setter == nullptr || setter->getType() != CALCULATION) {
      return false;
    }

    auto expression = ExecutionNode::castTo<CalculationNode const*>(setter)->expression();

    if (expression == nullptr) {
      return false;
    }
    if (!expression->isConstant()) {
      return false;
    }
  }

  return true;
}

bool SubqueryNode::mayAccessCollections() {
  if (_plan->getAst()->functionsMayAccessDocuments()) {
    // if the query contains any calls to functions that MAY access any
    // document, then we count this as a "yes"
    return true;
  }

  TRI_ASSERT(_subquery != nullptr);

  // if the subquery contains any of these nodes, it may access data from
  // a collection
  std::vector<ExecutionNode::NodeType> const types = {ExecutionNode::ENUMERATE_IRESEARCH_VIEW,
                                                      ExecutionNode::ENUMERATE_COLLECTION,
                                                      ExecutionNode::INDEX,
                                                      ExecutionNode::INSERT,
                                                      ExecutionNode::UPDATE,
                                                      ExecutionNode::REPLACE,
                                                      ExecutionNode::REMOVE,
                                                      ExecutionNode::UPSERT,
                                                      ExecutionNode::TRAVERSAL,
                                                      ExecutionNode::SHORTEST_PATH,
                                                      ExecutionNode::K_SHORTEST_PATHS};

  ::arangodb::containers::SmallVector<ExecutionNode*>::allocator_type::arena_type a;
  ::arangodb::containers::SmallVector<ExecutionNode*> nodes{a};

  NodeFinder<std::vector<ExecutionNode::NodeType>> finder(types, nodes, true);
  _subquery->walk(finder);

  if (!nodes.empty()) {
    return true;
  }

  return false;
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> SubqueryNode::createBlock(
    ExecutionEngine& engine,
    std::unordered_map<ExecutionNode*, ExecutionBlock*> const& cache) const {
  auto const it = cache.find(getSubquery());
  TRI_ASSERT(it != cache.end());
  auto subquery = it->second;
  TRI_ASSERT(subquery != nullptr);

  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  auto inputRegisters = std::make_shared<std::unordered_set<RegisterId>>();
  auto outputRegisters = std::make_shared<std::unordered_set<RegisterId>>();

  auto outVar = getRegisterPlan()->varInfo.find(_outVariable->id);
  TRI_ASSERT(outVar != getRegisterPlan()->varInfo.end());
  RegisterId outReg = outVar->second.registerId;
  outputRegisters->emplace(outReg);

  // The const_cast has been taken from previous implementation.
  SubqueryExecutorInfos infos(inputRegisters, outputRegisters,
                              getRegisterPlan()->nrRegs[previousNode->getDepth()],
                              getRegisterPlan()->nrRegs[getDepth()],
                              getRegsToClear(), calcRegsToKeep(), *subquery,
                              outReg, const_cast<SubqueryNode*>(this)->isConst());
  if (isModificationSubquery()) {
    return std::make_unique<ExecutionBlockImpl<SubqueryExecutor<true>>>(&engine, this,
                                                                        std::move(infos));
  } else {
    return std::make_unique<ExecutionBlockImpl<SubqueryExecutor<false>>>(&engine, this,
                                                                         std::move(infos));
  }
}

ExecutionNode* SubqueryNode::clone(ExecutionPlan* plan, bool withDependencies,
                                   bool withProperties) const {
  auto outVariable = _outVariable;

  if (withProperties) {
    outVariable = plan->getAst()->variables()->createVariable(outVariable);
  }
  auto c = std::make_unique<SubqueryNode>(plan, _id, _subquery->clone(plan, true, withProperties),
                                          outVariable);

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

/// @brief whether or not the subquery is a data-modification operation
bool SubqueryNode::isModificationSubquery() const {
  std::vector<ExecutionNode*> stack({_subquery});

  while (!stack.empty()) {
    ExecutionNode* current = stack.back();

    if (current->isModificationNode()) {
      return true;
    }

    stack.pop_back();

    current->dependencies(stack);
  }

  return false;
}

/// @brief replace the out variable, so we can adjust the name.
void SubqueryNode::replaceOutVariable(Variable const* var) {
  _outVariable = var;
}

/// @brief estimateCost
CostEstimate SubqueryNode::estimateCost() const {
  TRI_ASSERT(!_dependencies.empty());
  CostEstimate subEstimate = _subquery->getCost();

  CostEstimate estimate = _dependencies.at(0)->getCost();
  estimate.estimatedCost += estimate.estimatedNrItems * subEstimate.estimatedCost;
  return estimate;
}

/// @brief helper struct to find all (outer) variables used in a SubqueryNode
struct SubqueryVarUsageFinder final : public WalkerWorker<ExecutionNode> {
  ::arangodb::containers::HashSet<Variable const*> _usedLater;
  ::arangodb::containers::HashSet<Variable const*> _valid;

  SubqueryVarUsageFinder() {}

  ~SubqueryVarUsageFinder() = default;

  bool before(ExecutionNode* en) override final {
    // Add variables used here to _usedLater:
    en->getVariablesUsedHere(_usedLater);
    return false;
  }

  void after(ExecutionNode* en) override final {
    // Add variables set here to _valid:
    for (auto& v : en->getVariablesSetHere()) {
      _valid.insert(v);
    }
  }

  bool enterSubquery(ExecutionNode*, ExecutionNode* sub) override final {
    SubqueryVarUsageFinder subfinder;
    sub->walk(subfinder);

    // keep track of all variables used by a (dependent) subquery
    // this is, all variables in the subqueries _usedLater that are not in
    // _valid

    // create the set difference. note: cannot use std::set_difference as our
    // sets are NOT sorted
    for (auto var : subfinder._usedLater) {
      if (_valid.find(var) != _valid.end()) {
        _usedLater.insert(var);
      }
    }
    return false;
  }
};

/// @brief getVariablesUsedHere, modifying the set in-place
void SubqueryNode::getVariablesUsedHere(::arangodb::containers::HashSet<Variable const*>& vars) const {
  SubqueryVarUsageFinder finder;
  _subquery->walk(finder);

  for (auto var : finder._usedLater) {
    if (finder._valid.find(var) == finder._valid.end()) {
      vars.insert(var);
    }
  }
}

/// @brief is the node determistic?
struct DeterministicFinder final : public WalkerWorker<ExecutionNode> {
  bool _isDeterministic = true;

  DeterministicFinder() : _isDeterministic(true) {}
  ~DeterministicFinder() = default;

  bool enterSubquery(ExecutionNode*, ExecutionNode*) override final {
    return false;
  }

  bool before(ExecutionNode* node) override final {
    if (!node->isDeterministic()) {
      _isDeterministic = false;
      return true;
    }
    return false;
  }
};

bool SubqueryNode::isDeterministic() {
  DeterministicFinder finder;
  _subquery->walk(finder);
  return finder._isDeterministic;
}

SubqueryNode::SubqueryNode(ExecutionPlan* plan, size_t id,
                           ExecutionNode* subquery, Variable const* outVariable)
    : ExecutionNode(plan, id), _subquery(subquery), _outVariable(outVariable) {
  TRI_ASSERT(_subquery != nullptr);
  TRI_ASSERT(_outVariable != nullptr);
}

ExecutionNode::NodeType SubqueryNode::getType() const { return SUBQUERY; }

Variable const* SubqueryNode::outVariable() const { return _outVariable; }

ExecutionNode* SubqueryNode::getSubquery() const { return _subquery; }

void SubqueryNode::setSubquery(ExecutionNode* subquery, bool forceOverwrite) {
  TRI_ASSERT(subquery != nullptr);
  TRI_ASSERT((forceOverwrite && _subquery != nullptr) ||
             (!forceOverwrite && _subquery == nullptr));
  _subquery = subquery;
}

std::vector<Variable const*> SubqueryNode::getVariablesSetHere() const {
  return std::vector<Variable const*>{_outVariable};
}

FilterNode::FilterNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _inVariable(Variable::varFromVPack(plan->getAst(), base, "inVariable")) {}

/// @brief toVelocyPack, for FilterNode
void FilterNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                    std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);

  nodes.add(VPackValue("inVariable"));
  _inVariable->toVelocyPack(nodes);

  // And close it:
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> FilterNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);
  RegisterId inputRegister = variableToRegisterId(_inVariable);

  FilterExecutorInfos infos(inputRegister,
                            getRegisterPlan()->nrRegs[previousNode->getDepth()],
                            getRegisterPlan()->nrRegs[getDepth()],
                            getRegsToClear(), calcRegsToKeep());
  return std::make_unique<ExecutionBlockImpl<FilterExecutor>>(&engine, this,
                                                              std::move(infos));
}

ExecutionNode* FilterNode::clone(ExecutionPlan* plan, bool withDependencies,
                                 bool withProperties) const {
  auto inVariable = _inVariable;

  if (withProperties) {
    inVariable = plan->getAst()->variables()->createVariable(inVariable);
  }

  auto c = std::make_unique<FilterNode>(plan, _id, inVariable);

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

/// @brief estimateCost
CostEstimate FilterNode::estimateCost() const {
  TRI_ASSERT(!_dependencies.empty());

  // We are pessimistic here by not reducing the nrItems. However, in the
  // worst case the filter does not reduce the items at all. Furthermore,
  // no optimizer rule introduces FilterNodes, thus it is not important
  // that they appear to lower the costs. Note that contrary to this,
  // an IndexNode does lower the costs, it also has a better idea
  // to what extent the number of items is reduced. On the other hand it
  // is important that a FilterNode produces additional costs, otherwise
  // the rule throwing away a FilterNode that is already covered by an
  // IndexNode cannot reduce the costs.
  CostEstimate estimate = _dependencies.at(0)->getCost();
  estimate.estimatedCost += estimate.estimatedNrItems;
  return estimate;
}

FilterNode::FilterNode(ExecutionPlan* plan, size_t id, Variable const* inVariable)
    : ExecutionNode(plan, id), _inVariable(inVariable) {
  TRI_ASSERT(_inVariable != nullptr);
}

ExecutionNode::NodeType FilterNode::getType() const { return FILTER; }

void FilterNode::getVariablesUsedHere(::arangodb::containers::HashSet<const Variable*>& vars) const {
  vars.emplace(_inVariable);
}

Variable const* FilterNode::inVariable() const { return _inVariable; }

ReturnNode::ReturnNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      _inVariable(Variable::varFromVPack(plan->getAst(), base, "inVariable")),
      _count(VelocyPackHelper::getBooleanValue(base, "count", false)) {}

/// @brief toVelocyPack, for ReturnNode
void ReturnNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                    std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);

  nodes.add(VPackValue("inVariable"));
  _inVariable->toVelocyPack(nodes);
  nodes.add("count", VPackValue(_count));

  // And close it:
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> ReturnNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inputRegister = variableToRegisterId(_inVariable);

  bool const isRoot = plan()->root() == this;

  bool const isDBServer = arangodb::ServerState::instance()->isDBServer();

  bool const returnInheritedResults = isRoot && !isDBServer;

  // This is an important performance improvement:
  // If we have inherited results, we do move the block through
  // and do not modify it in any way.
  // In the other case it is important to shrink the matrix to exactly
  // one register that is stored within the DOCVEC.
  RegisterId const numberInputRegisters =
      getRegisterPlan()->nrRegs[previousNode->getDepth()];
  RegisterId const numberOutputRegisters =
      returnInheritedResults ? getRegisterPlan()->nrRegs[getDepth()] : 1;

  if (returnInheritedResults) {
    return std::make_unique<ExecutionBlockImpl<IdExecutor<BlockPassthrough::Enable, void>>>(
        &engine, this, inputRegister, _count);
  } else {
    TRI_ASSERT(!returnInheritedResults);
    ReturnExecutorInfos infos(inputRegister, numberInputRegisters,
                              numberOutputRegisters, _count);

    return std::make_unique<ExecutionBlockImpl<ReturnExecutor>>(&engine, this,
                                                                std::move(infos));
  }
}

/// @brief clone ExecutionNode recursively
ExecutionNode* ReturnNode::clone(ExecutionPlan* plan, bool withDependencies,
                                 bool withProperties) const {
  auto inVariable = _inVariable;

  if (withProperties) {
    inVariable = plan->getAst()->variables()->createVariable(inVariable);
  }

  auto c = std::make_unique<ReturnNode>(plan, _id, inVariable);

  if (_count) {
    c->setCount();
  }

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

/// @brief estimateCost
CostEstimate ReturnNode::estimateCost() const {
  TRI_ASSERT(!_dependencies.empty());
  CostEstimate estimate = _dependencies.at(0)->getCost();
  estimate.estimatedCost += estimate.estimatedNrItems;
  return estimate;
}

ReturnNode::ReturnNode(ExecutionPlan* plan, size_t id, Variable const* inVariable)
    : ExecutionNode(plan, id), _inVariable(inVariable), _count(false) {
  TRI_ASSERT(_inVariable != nullptr);
}

ExecutionNode::NodeType ReturnNode::getType() const { return RETURN; }

void ReturnNode::setCount() { _count = true; }

void ReturnNode::getVariablesUsedHere(::arangodb::containers::HashSet<const Variable*>& vars) const {
  vars.emplace(_inVariable);
}

Variable const* ReturnNode::inVariable() const { return _inVariable; }

void ReturnNode::inVariable(Variable const* v) { _inVariable = v; }

/// @brief toVelocyPack, for NoResultsNode
void NoResultsNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                       std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);

  // And close it
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> NoResultsNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);
  ExecutorInfos infos(arangodb::aql::make_shared_unordered_set(),
                      arangodb::aql::make_shared_unordered_set(),
                      getRegisterPlan()->nrRegs[previousNode->getDepth()],
                      getRegisterPlan()->nrRegs[getDepth()], getRegsToClear(),
                      calcRegsToKeep());
  return std::make_unique<ExecutionBlockImpl<NoResultsExecutor>>(&engine, this,
                                                                 std::move(infos));
}

/// @brief estimateCost, the cost of a NoResults is nearly 0
CostEstimate NoResultsNode::estimateCost() const {
  CostEstimate estimate = CostEstimate::empty();
  estimate.estimatedCost = 0.5;  // just to make it non-zero
  return estimate;
}

NoResultsNode::NoResultsNode(ExecutionPlan* plan, size_t id)
    : ExecutionNode(plan, id) {}

NoResultsNode::NoResultsNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base) {}

ExecutionNode::NodeType NoResultsNode::getType() const { return NORESULTS; }

ExecutionNode* NoResultsNode::clone(ExecutionPlan* plan, bool withDependencies,
                                    bool withProperties) const {
  return cloneHelper(std::make_unique<NoResultsNode>(plan, _id),
                     withDependencies, withProperties);
}

SortElement::SortElement(Variable const* v, bool asc)
    : var(v), ascending(asc) {}

SortElement::SortElement(Variable const* v, bool asc, std::vector<std::string> const& path)
    : var(v), ascending(asc), attributePath(path) {}

std::string SortElement::toString() const {
  std::string result("$");
  result += std::to_string(var->id);
  for (auto const& it : attributePath) {
    result += "." + it;
  }
  return result;
}

EnumerateCollectionNode::EnumerateCollectionNode(ExecutionPlan* plan, size_t id,
                                                 aql::Collection const* collection,
                                                 Variable const* outVariable,
                                                 bool random, IndexHint const& hint)
    : ExecutionNode(plan, id),
      DocumentProducingNode(outVariable),
      CollectionAccessingNode(collection),
      _random(random),
      _hint(hint) {}

ExecutionNode::NodeType EnumerateCollectionNode::getType() const {
  return ENUMERATE_COLLECTION;
}

IndexHint const& EnumerateCollectionNode::hint() const { return _hint; }

SortInformation::Match SortInformation::isCoveredBy(SortInformation const& other) {
  if (!isValid || !other.isValid) {
    return unequal;
  }

  if (isComplex || other.isComplex) {
    return unequal;
  }

  size_t const n = criteria.size();
  for (size_t i = 0; i < n; ++i) {
    if (other.criteria.size() <= i) {
      return otherLessAccurate;
    }

    auto ours = criteria[i];
    auto theirs = other.criteria[i];

    if (std::get<2>(ours) != std::get<2>(theirs)) {
      // sort order is different
      return unequal;
    }

    if (std::get<1>(ours) != std::get<1>(theirs)) {
      // sort criterion is different
      return unequal;
    }
  }

  if (other.criteria.size() > n) {
    return ourselvesLessAccurate;
  }

  return allEqual;
}

namespace {
const char* MATERIALIZE_NODE_IN_NM_COL_PARAM = "inNmColPtr";
const char* MATERIALIZE_NODE_IN_NM_DOC_PARAM = "inNmDocId";
const char* MATERIALIZE_NODE_OUT_VARIABLE_PARAM = "outVariable";
}

MaterializeNode* materialize::createMaterializeNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base) {
  if (base.hasKey(MATERIALIZE_NODE_IN_NM_COL_PARAM)) {
    return new MaterializeMultiNode(plan, base);
  }
  return new MaterializeSingleNode(plan, base);
}

MaterializeNode::MaterializeNode(ExecutionPlan* plan, size_t id,
                                 aql::Variable const& inDocId,
                                 aql::Variable const& outVariable)
  : ExecutionNode(plan, id), _inNonMaterializedDocId(&inDocId), _outVariable(&outVariable) {}

MaterializeNode::MaterializeNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
  : ExecutionNode(plan, base),
    _inNonMaterializedDocId(aql::Variable::varFromVPack(plan->getAst(), base, MATERIALIZE_NODE_IN_NM_DOC_PARAM, true)),
    _outVariable(aql::Variable::varFromVPack(plan->getAst(), base, MATERIALIZE_NODE_OUT_VARIABLE_PARAM)) {}

void MaterializeNode::toVelocyPackHelper(arangodb::velocypack::Builder& nodes, unsigned flags,
                                         std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  aql::ExecutionNode::toVelocyPackHelperGeneric(nodes, flags, seen);

  nodes.add(VPackValue(MATERIALIZE_NODE_IN_NM_DOC_PARAM));
  _inNonMaterializedDocId->toVelocyPack(nodes);

  nodes.add(VPackValue(MATERIALIZE_NODE_OUT_VARIABLE_PARAM));
  _outVariable->toVelocyPack(nodes);
}

CostEstimate MaterializeNode::estimateCost() const {
  if (_dependencies.empty()) {
    // we should always have dependency as we need input for materializing
    TRI_ASSERT(false);
    return aql::CostEstimate::empty();
  }
  aql::CostEstimate estimate = _dependencies[0]->getCost();
  // we will materialize all output of our dependency
  estimate.estimatedCost += estimate.estimatedNrItems;
  return estimate;
}

void MaterializeNode::getVariablesUsedHere(::arangodb::containers::HashSet<Variable const*>& vars) const {
  vars.emplace(_inNonMaterializedDocId);
}

std::vector<Variable const*> MaterializeNode::getVariablesSetHere() const {
  return std::vector<Variable const*>{_outVariable};
}

MaterializeMultiNode::MaterializeMultiNode(ExecutionPlan* plan, size_t id,
                                           aql::Variable const& inColPtr,
                                           aql::Variable const& inDocId,
                                           aql::Variable const& outVariable)
  : MaterializeNode(plan, id, inDocId, outVariable),
    _inNonMaterializedColPtr(&inColPtr) {}

MaterializeMultiNode::MaterializeMultiNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
  : MaterializeNode(plan, base),
    _inNonMaterializedColPtr(aql::Variable::varFromVPack(plan->getAst(), base, MATERIALIZE_NODE_IN_NM_COL_PARAM, true)) {}

void MaterializeMultiNode::toVelocyPackHelper(arangodb::velocypack::Builder& nodes, unsigned flags,
                                              std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  MaterializeNode::toVelocyPackHelper(nodes, flags, seen);

  nodes.add(VPackValue(MATERIALIZE_NODE_IN_NM_COL_PARAM));
  _inNonMaterializedColPtr->toVelocyPack(nodes);

  nodes.close();
}

std::unique_ptr<ExecutionBlock> MaterializeMultiNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {

  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inNmColPtrRegId;
  {
    auto it = getRegisterPlan()->varInfo.find(_inNonMaterializedColPtr->id);
    TRI_ASSERT(it != getRegisterPlan()->varInfo.end());
    inNmColPtrRegId = it->second.registerId;
  }
  RegisterId inNmDocIdRegId;
  {
    auto it = getRegisterPlan()->varInfo.find(_inNonMaterializedDocId->id);
    TRI_ASSERT(it != getRegisterPlan()->varInfo.end());
    inNmDocIdRegId = it->second.registerId;
  }
  RegisterId outDocumentRegId;
  {
    auto it = getRegisterPlan()->varInfo.find(_outVariable->id);
    TRI_ASSERT(it != getRegisterPlan()->varInfo.end());
    outDocumentRegId = it->second.registerId;
  }
  TRI_ASSERT(engine.getQuery());

  return std::make_unique<ExecutionBlockImpl<MaterializeExecutor<decltype(inNmColPtrRegId)>>>(&engine, this,
     MaterializerExecutorInfos(getRegisterPlan()->nrRegs[previousNode->getDepth()],
                               getRegisterPlan()->nrRegs[getDepth()], getRegsToClear(),
                               calcRegsToKeep(), inNmColPtrRegId, inNmDocIdRegId,
                               outDocumentRegId, engine.getQuery()->trx()));
}

ExecutionNode* MaterializeMultiNode::clone(ExecutionPlan* plan, bool withDependencies, bool withProperties) const {
  TRI_ASSERT(plan);

  auto* outVariable = _outVariable;
  auto* inNonMaterializedDocId = _inNonMaterializedDocId;
  auto* inNonMaterializedColId = _inNonMaterializedColPtr;

  if (withProperties) {
    outVariable = plan->getAst()->variables()->createVariable(outVariable);
    inNonMaterializedDocId = plan->getAst()->variables()->createVariable(inNonMaterializedDocId);
    inNonMaterializedColId = plan->getAst()->variables()->createVariable(inNonMaterializedColId);
  }

  auto c = std::make_unique<MaterializeMultiNode>(plan, _id, *inNonMaterializedColId, *inNonMaterializedDocId, *outVariable);
  return cloneHelper(std::move(c), withDependencies, withProperties);
}

void MaterializeMultiNode::getVariablesUsedHere(::arangodb::containers::HashSet<Variable const*>& vars) const {
  // call base class method
  MaterializeNode::getVariablesUsedHere(vars);

  vars.emplace(_inNonMaterializedColPtr);
}

MaterializeSingleNode::MaterializeSingleNode(ExecutionPlan* plan, size_t id, aql::Collection const* collection,
                                             aql::Variable const& inDocId, aql::Variable const& outVariable)
  : MaterializeNode(plan, id, inDocId, outVariable), CollectionAccessingNode(collection) {}

MaterializeSingleNode::MaterializeSingleNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
  : MaterializeNode(plan, base), CollectionAccessingNode(plan, base) {}

void MaterializeSingleNode::toVelocyPackHelper(arangodb::velocypack::Builder& nodes, unsigned flags,
                                              std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  MaterializeNode::toVelocyPackHelper(nodes, flags, seen);

  // add collection information
  CollectionAccessingNode::toVelocyPack(nodes, flags);

  nodes.close();
}

std::unique_ptr<ExecutionBlock> MaterializeSingleNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {

  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);
  RegisterId inNmDocIdRegId;
  {
    auto it = getRegisterPlan()->varInfo.find(_inNonMaterializedDocId->id);
    TRI_ASSERT(it != getRegisterPlan()->varInfo.end());
    inNmDocIdRegId = it->second.registerId;
  }
  RegisterId outDocumentRegId;
  {
    auto it = getRegisterPlan()->varInfo.find(_outVariable->id);
    TRI_ASSERT(it != getRegisterPlan()->varInfo.end());
    outDocumentRegId = it->second.registerId;
  }
  TRI_ASSERT(engine.getQuery());
  auto const& name = _collection->name();

  return std::make_unique<ExecutionBlockImpl<MaterializeExecutor<decltype(name)>>>(&engine, this,
    MaterializerExecutorInfos<decltype(name)>(getRegisterPlan()->nrRegs[previousNode->getDepth()],
                                              getRegisterPlan()->nrRegs[getDepth()], getRegsToClear(),
                                              calcRegsToKeep(), _collection->name(), inNmDocIdRegId,
                                              outDocumentRegId, engine.getQuery()->trx()));
}

ExecutionNode* MaterializeSingleNode::clone(ExecutionPlan * plan, bool withDependencies, bool withProperties) const {
  TRI_ASSERT(plan);

  auto* outVariable = _outVariable;
  auto* inNonMaterializedDocId = _inNonMaterializedDocId;

  if (withProperties) {
    outVariable = plan->getAst()->variables()->createVariable(outVariable);
    inNonMaterializedDocId = plan->getAst()->variables()->createVariable(inNonMaterializedDocId);
  }

  auto c = std::make_unique<MaterializeSingleNode>(plan, _id, _collection, *inNonMaterializedDocId, *outVariable);
  CollectionAccessingNode::cloneInto(*c);
  return cloneHelper(std::move(c), withDependencies, withProperties);
}
