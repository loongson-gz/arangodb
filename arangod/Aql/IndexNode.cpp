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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "IndexNode.h"

#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/Condition.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Expression.h"
#include "Aql/IndexExecutor.h"
#include "Aql/Query.h"
#include "Aql/RegisterPlan.h"
#include "Aql/SingleRowFetcher.h"
#include "Basics/AttributeNameParser.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Indexes/Index.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Methods.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

/// @brief constructor
IndexNode::IndexNode(ExecutionPlan* plan, size_t id,
                     Collection const* collection, Variable const* outVariable,
                     std::vector<transaction::Methods::IndexHandle> const& indexes,
                     std::unique_ptr<Condition> condition, IndexIteratorOptions const& opts)
    : ExecutionNode(plan, id),
      DocumentProducingNode(outVariable),
      CollectionAccessingNode(collection),
      _indexes(indexes),
      _condition(std::move(condition)),
      _needsGatherNodeSort(false),
      _options(opts),
      _outNonMaterializedDocId(nullptr) {
  TRI_ASSERT(_condition != nullptr);

  initIndexCoversProjections();
}

/// @brief constructor for IndexNode
IndexNode::IndexNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      DocumentProducingNode(plan, base),
      CollectionAccessingNode(plan, base),
      _indexes(),
      _needsGatherNodeSort(
          basics::VelocyPackHelper::getBooleanValue(base, "needsGatherNodeSort", false)),
      _options(),
      _outNonMaterializedDocId(
          aql::Variable::varFromVPack(plan->getAst(), base, "outNmDocId", true)) {
  _options.sorted = basics::VelocyPackHelper::getBooleanValue(base, "sorted", true);
  _options.ascending =
      basics::VelocyPackHelper::getBooleanValue(base, "ascending", false);
  _options.evaluateFCalls =
      basics::VelocyPackHelper::getBooleanValue(base, "evalFCalls", true);
  _options.limit = basics::VelocyPackHelper::getNumericValue(base, "limit", 0);

  if (_options.sorted && base.isObject() && base.get("reverse").isBool()) {
    // legacy
    _options.sorted = true;
    _options.ascending = !(base.get("reverse").getBool());
  }

  VPackSlice indexes = base.get("indexes");

  if (!indexes.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "\"indexes\" attribute should be an array");
  }

  _indexes.reserve(indexes.length());

  auto trx = plan->getAst()->query()->trx();
  for (VPackSlice it : VPackArrayIterator(indexes)) {
    std::string iid = it.get("id").copyString();
    _indexes.emplace_back(trx->getIndexByIdentifier(_collection->name(), iid));
  }

  VPackSlice condition = base.get("condition");
  if (!condition.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER, "\"condition\" attribute should be an object");
  }

  _condition = Condition::fromVPack(plan, condition);

  TRI_ASSERT(_condition != nullptr);

  initIndexCoversProjections();

  if (_outNonMaterializedDocId != nullptr) {
    auto const* vars = plan->getAst()->variables();
    TRI_ASSERT(vars);

    auto const indexIdSlice = base.get("indexIdOfVars");
    if (!indexIdSlice.isNumber<TRI_idx_iid_t>()) {
      THROW_ARANGO_EXCEPTION_FORMAT(
          TRI_ERROR_BAD_PARAMETER, "\"indexIdOfVars\" %s should be a number",
            indexIdSlice.toString().c_str());
    }

    auto const indexId = indexIdSlice.getNumber<TRI_idx_iid_t>();

    auto const indexValuesVarsSlice = base.get("IndexValuesVars");
    if (!indexValuesVarsSlice.isArray()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "\"IndexValuesVars\" attribute should be an array");
    }
    std::unordered_map<size_t, Variable const*> indexValuesVars;
    indexValuesVars.reserve(indexValuesVarsSlice.length());
    for (auto const indVar : velocypack::ArrayIterator(indexValuesVarsSlice)) {
      auto const fieldNumberSlice = indVar.get("fieldNumber");
      if (!fieldNumberSlice.isNumber<size_t>()) {
        THROW_ARANGO_EXCEPTION_FORMAT(
            TRI_ERROR_BAD_PARAMETER, "\"IndexValuesVars[*].fieldNumber\" %s should be a number",
              fieldNumberSlice.toString().c_str());
      }
      auto const fieldNumber = fieldNumberSlice.getNumber<size_t>();

      auto const varIdSlice = indVar.get("id");
      if (!varIdSlice.isNumber<aql::VariableId>()) {
        THROW_ARANGO_EXCEPTION_FORMAT(
            TRI_ERROR_BAD_PARAMETER, "\"IndexValuesVars[*].id\" variable id %s should be a number",
              varIdSlice.toString().c_str());
      }

      auto const varId = varIdSlice.getNumber<aql::VariableId>();
      auto const* var = vars->getVariable(varId);

      if (!var) {
        THROW_ARANGO_EXCEPTION_FORMAT(
            TRI_ERROR_BAD_PARAMETER, "\"IndexValuesVars[*].id\" unable to find variable by id %d",
              varId);
      }
      indexValuesVars.emplace(fieldNumber, var);
    }
    _outNonMaterializedIndVars.first = indexId;
    _outNonMaterializedIndVars.second = std::move(indexValuesVars);
  }
}

/// @brief called to build up the matching positions of the index values for
/// the projection attributes (if any)
void IndexNode::initIndexCoversProjections() {
  _coveringIndexAttributePositions.clear();

  if (_indexes.empty()) {
    // no indexes used
    return;
  }

  // cannot apply the optimization if we use more than one different index
  auto const& idx = _indexes[0];
  for (size_t i = 1; i < _indexes.size(); ++i) {
    if (_indexes[i] != idx) {
      // different index used => optimization not possible
      return;
    }
  }

  // note that we made sure that if we have multiple index instances, they
  // are actually all of the same index

  if (!idx->hasCoveringIterator()) {
    // index does not have a covering index iterator
    return;
  }

  // check if we can use covering indexes
  auto const& fields = idx->coveredFields();

  if (fields.size() < projections().size()) {
    // we will not be able to satisfy all requested projections with this index
    return;
  }

  std::vector<size_t> coveringAttributePositions;
  // test if the index fields are the same fields as used in the projection
  std::string result;
  for (auto const& it : projections()) {
    bool found = false;
    for (size_t j = 0; j < fields.size(); ++j) {
      result.clear();
      TRI_AttributeNamesToString(fields[j], result, false);
      if (result == it) {
        found = true;
        coveringAttributePositions.emplace_back(j);
        break;
      }
    }
    if (!found) {
      return;
    }
  }

  _coveringIndexAttributePositions = std::move(coveringAttributePositions);
  _options.forceProjection = true;
}

void IndexNode::planNodeRegisters(
    std::vector<aql::RegisterId>& nrRegsHere, std::vector<aql::RegisterId>& nrRegs,
    std::unordered_map<aql::VariableId, aql::VarInfo>& varInfo,
    unsigned int& totalNrRegs, unsigned int depth) const {
  // create a copy of the last value here
  // this is required because back returns a reference and emplace/push_back
  // may invalidate all references
  auto regsCount = nrRegs.back();
  nrRegs.emplace_back(regsCount + 1);
  nrRegsHere.emplace_back(1);

  if (isLateMaterialized()) {
    varInfo.emplace(_outNonMaterializedDocId->id, aql::VarInfo(depth, totalNrRegs++));
    // plan registers for index references
    for (auto const& var : _outNonMaterializedIndVars.second) {
      ++nrRegsHere[depth];
      ++nrRegs[depth];
      varInfo.emplace(var.second->id, aql::VarInfo(depth, totalNrRegs++));
    }
  } else {
    varInfo.emplace(_outVariable->id, aql::VarInfo(depth, totalNrRegs++));
  }
}

/// @brief toVelocyPack, for IndexNode
void IndexNode::toVelocyPackHelper(VPackBuilder& builder, unsigned flags,
                                   std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(builder, flags, seen);

  // add outvariable and projections
  DocumentProducingNode::toVelocyPack(builder, flags);

  // add collection information
  CollectionAccessingNode::toVelocyPack(builder, flags);

  // Now put info about vocbase and cid in there
  builder.add("needsGatherNodeSort", VPackValue(_needsGatherNodeSort));
  builder.add("indexCoversProjections",
              VPackValue(!_coveringIndexAttributePositions.empty()));

  builder.add(VPackValue("indexes"));
  {
    VPackArrayBuilder guard(&builder);
    for (auto const& index : _indexes) {
      index->toVelocyPack(builder, Index::makeFlags(Index::Serialize::Estimates));
    }
  }
  builder.add(VPackValue("condition"));
  _condition->toVelocyPack(builder, flags);
  // IndexIteratorOptions
  builder.add("sorted", VPackValue(_options.sorted));
  builder.add("ascending", VPackValue(_options.ascending));
  builder.add("reverse", VPackValue(!_options.ascending));  // legacy
  builder.add("evalFCalls", VPackValue(_options.evaluateFCalls));
  builder.add("limit", VPackValue(_options.limit));

  if (isLateMaterialized()) {
    builder.add(VPackValue("outNmDocId"));
    _outNonMaterializedDocId->toVelocyPack(builder);

    builder.add("indexIdOfVars", VPackValue(_outNonMaterializedIndVars.first));
    // container _indexes contains a few items
    auto indIt = std::find_if(_indexes.cbegin(), _indexes.cend(), [this](auto const& index) {
      return index->id() == _outNonMaterializedIndVars.first;
    });
    TRI_ASSERT(indIt != _indexes.cend());
    auto const& fields = (*indIt)->fields();
    VPackArrayBuilder arrayScope(&builder, "IndexValuesVars");
    for (auto const& indVar : _outNonMaterializedIndVars.second) {
      VPackObjectBuilder objectScope(&builder);
      builder.add("fieldNumber", VPackValue(indVar.first));
      builder.add("id", VPackValue(indVar.second->id));
      builder.add("name", VPackValue(indVar.second->name)); // for explainer.js
      std::string fieldName;
      TRI_ASSERT(indVar.first < fields.size());
      basics::TRI_AttributeNamesToString(fields[indVar.first], fieldName, true);
      builder.add("field", VPackValue(fieldName)); // for explainer.js
    }
  }

  // And close it:
  builder.close();
}

/// @brief adds a UNIQUE() to a dynamic IN condition
arangodb::aql::AstNode* IndexNode::makeUnique(arangodb::aql::AstNode* node,
                                              transaction::Methods* trx) const {
  if (node->type != arangodb::aql::NODE_TYPE_ARRAY || node->numMembers() >= 2) {
    // an non-array or an array with more than 1 member
    auto ast = _plan->getAst();
    auto array = _plan->getAst()->createNodeArray();
    array->addMember(node);
   
    TRI_ASSERT(trx != nullptr);

    // Here it does not matter which index we choose for the isSorted/isSparse
    // check, we need them all sorted here.

    auto idx = _indexes.at(0);
    if (!idx) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "The index id cannot be empty.");
    }
    
    if (idx->sparse() || idx->isSorted()) {
      // the index is sorted. we need to use SORTED_UNIQUE to get the
      // result back in index order
      return ast->createNodeFunctionCall(TRI_CHAR_LENGTH_PAIR("SORTED_UNIQUE"), array);
    }
    // a regular UNIQUE will do
    return ast->createNodeFunctionCall(TRI_CHAR_LENGTH_PAIR("UNIQUE"), array);
  }

  // presumably an array with no or a single member
  return node;
}

void IndexNode::initializeOnce(bool hasV8Expression, std::vector<Variable const*>& inVars,
                               std::vector<RegisterId>& inRegs,
                               std::vector<std::unique_ptr<NonConstExpression>>& nonConstExpressions,
                               transaction::Methods* trxPtr) const {
  // instantiate expressions:
  auto instantiateExpression = [&](AstNode* a, std::vector<size_t>&& idxs) -> void {
    // all new AstNodes are registered with the Ast in the Query
    auto e = std::make_unique<Expression>(_plan, _plan->getAst(), a);

    TRI_IF_FAILURE("IndexBlock::initialize") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    hasV8Expression |= e->willUseV8();

    ::arangodb::containers::HashSet<Variable const*> innerVars;
    e->variables(innerVars);

    nonConstExpressions.emplace_back(
        std::make_unique<NonConstExpression>(std::move(e), std::move(idxs)));

    for (auto const& v : innerVars) {
      inVars.emplace_back(v);
      auto it = getRegisterPlan()->varInfo.find(v->id);
      TRI_ASSERT(it != getRegisterPlan()->varInfo.cend());
      TRI_ASSERT(it->second.registerId < RegisterPlan::MaxRegisterId);
      inRegs.emplace_back(it->second.registerId);
    }
  };

  if (_condition->root() != nullptr) {
    auto outVariable = _outVariable;
    std::function<bool(AstNode const*)> hasOutVariableAccess = [&](AstNode const* node) -> bool {
      if (node->isAttributeAccessForVariable(outVariable, true)) {
        return true;
      }

      bool accessedInSubtree = false;
      for (size_t i = 0; i < node->numMembers() && !accessedInSubtree; i++) {
        accessedInSubtree = hasOutVariableAccess(node->getMemberUnchecked(i));
      }

      return accessedInSubtree;
    };

    auto instFCallArgExpressions = [&](AstNode* fcall, std::vector<size_t>&& indexPath) {
      TRI_ASSERT(1 == fcall->numMembers());
      indexPath.emplace_back(0);  // for the arguments array
      AstNode* array = fcall->getMemberUnchecked(0);
      for (size_t k = 0; k < array->numMembers(); k++) {
        AstNode* child = array->getMemberUnchecked(k);
        if (!child->isConstant() && !hasOutVariableAccess(child)) {
          std::vector<size_t> idx = indexPath;
          idx.emplace_back(k);
          instantiateExpression(child, std::move(idx));

          TRI_IF_FAILURE("IndexBlock::initializeExpressions") {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
          }
        }
      }
    };

    // conditions can be of the form (a [<|<=|>|=>] b) && ...
    // in case of a geo spatial index a might take the form
    // of a GEO_* function. We might need to evaluate fcall arguments
    for (size_t i = 0; i < _condition->root()->numMembers(); ++i) {
      auto andCond = _condition->root()->getMemberUnchecked(i);
      for (size_t j = 0; j < andCond->numMembers(); ++j) {
        auto leaf = andCond->getMemberUnchecked(j);

        // FCALL at this level is most likely a geo index
        if (leaf->type == NODE_TYPE_FCALL) {
          instFCallArgExpressions(leaf, {i, j});
          continue;
        } else if (leaf->numMembers() != 2) {
          continue;
        }

        // We only support binary conditions
        TRI_ASSERT(leaf->numMembers() == 2);
        AstNode* lhs = leaf->getMember(0);
        AstNode* rhs = leaf->getMember(1);

        if (lhs->isAttributeAccessForVariable(outVariable, false)) {
          // Index is responsible for the left side, check if right side
          // has to be evaluated
          if (!rhs->isConstant()) {
            if (leaf->type == NODE_TYPE_OPERATOR_BINARY_IN) {
              rhs = makeUnique(rhs, trxPtr);
            }
            instantiateExpression(rhs, {i, j, 1});
            TRI_IF_FAILURE("IndexBlock::initializeExpressions") {
              THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
            }
          }
        } else {
          // Index is responsible for the right side, check if left side
          // has to be evaluated

          if (lhs->type == NODE_TYPE_FCALL && !options().evaluateFCalls) {
            // most likely a geo index condition
            instFCallArgExpressions(lhs, {i, j, 0});
          } else if (!lhs->isConstant()) {
            instantiateExpression(lhs, {i, j, 0});
            TRI_IF_FAILURE("IndexBlock::initializeExpressions") {
              THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
            }
          }
        }
      }
    }
  }
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> IndexNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  transaction::Methods* trxPtr = _plan->getAst()->query()->trx();

  trxPtr->pinData(_collection->id());

  bool hasV8Expression = false;
  /// @brief _inVars, a vector containing for each expression above
  /// a vector of Variable*, used to execute the expression
  std::vector<Variable const*> inVars;

  /// @brief _inRegs, a vector containing for each expression above
  /// a vector of RegisterId, used to execute the expression
  std::vector<RegisterId> inRegs;

  /// @brief _nonConstExpressions, list of all non const expressions, mapped
  /// by their _condition node path indexes
  std::vector<std::unique_ptr<NonConstExpression>> nonConstExpressions;

  initializeOnce(hasV8Expression, inVars, inRegs, nonConstExpressions, trxPtr);

  auto const firstOutputRegister = getNrInputRegisters();
  auto numIndVarsRegisters = static_cast<aql::RegisterCount>(_outNonMaterializedIndVars.second.size());
  TRI_ASSERT(0 == numIndVarsRegisters || isLateMaterialized());

  // We could be asked to produce only document id for later materialization or full document body at once
  aql::RegisterCount numDocumentRegs = 1;

  // if late materialized
  // We have one additional output register for each index variable which is used later, before
  // the output register for document id
  // These must of course fit in the available registers.
  // There may be unused registers reserved for later blocks.
  std::shared_ptr<std::unordered_set<aql::RegisterId>> writableOutputRegisters =
      aql::make_shared_unordered_set();
  writableOutputRegisters->reserve(numDocumentRegs + numIndVarsRegisters);
  for (aql::RegisterId reg = firstOutputRegister;
       reg < firstOutputRegister + numIndVarsRegisters + numDocumentRegs; ++reg) {
    writableOutputRegisters->emplace(reg);
  }

  TRI_ASSERT(writableOutputRegisters->size() == numDocumentRegs + numIndVarsRegisters);
  TRI_ASSERT(writableOutputRegisters->begin() != writableOutputRegisters->end());
  TRI_ASSERT(firstOutputRegister == *std::min_element(writableOutputRegisters->cbegin(),
                                                      writableOutputRegisters->cend()));

  auto const& varInfos = getRegisterPlan()->varInfo;
  IndexValuesRegisters outNonMaterializedIndRegs;
  outNonMaterializedIndRegs.first = _outNonMaterializedIndVars.first;
  outNonMaterializedIndRegs.second.reserve(_outNonMaterializedIndVars.second.size());
  std::transform(_outNonMaterializedIndVars.second.cbegin(), _outNonMaterializedIndVars.second.cend(),
                 std::inserter(outNonMaterializedIndRegs.second, outNonMaterializedIndRegs.second.end()),
                 [&varInfos](auto const& indVar) {
                   auto it = varInfos.find(indVar.second->id);
                   TRI_ASSERT(it != varInfos.cend());

                   return std::make_pair(indVar.first, it->second.registerId);
                 });

  IndexExecutorInfos infos(std::move(writableOutputRegisters),
                           getRegisterPlan()->nrRegs[previousNode->getDepth()],
                           firstOutputRegister,
                           getRegisterPlan()->nrRegs[getDepth()], getRegsToClear(),
                           calcRegsToKeep(), &engine, this->_collection, _outVariable,
                           (this->isVarUsedLater(_outVariable) || this->_filter != nullptr),
                           this->_filter.get(), this->projections(),
                           this->coveringIndexAttributePositions(),
                           EngineSelectorFeature::ENGINE->useRawDocumentPointers(),
                           std::move(nonConstExpressions), std::move(inVars),
                           std::move(inRegs), hasV8Expression, _condition->root(),
                           this->getIndexes(), _plan->getAst(), this->options(),
                           std::move(outNonMaterializedIndRegs));

  return std::make_unique<ExecutionBlockImpl<IndexExecutor>>(&engine, this, std::move(infos));
}

ExecutionNode* IndexNode::clone(ExecutionPlan* plan, bool withDependencies,
                                bool withProperties) const {
  auto outVariable = _outVariable;
  auto outNonMaterializedDocId = _outNonMaterializedDocId;
  auto outNonMaterializedIndVars = _outNonMaterializedIndVars;

  if (withProperties) {
    outVariable = plan->getAst()->variables()->createVariable(outVariable);
    if (outNonMaterializedDocId != nullptr) {
      outNonMaterializedDocId = plan->getAst()->variables()->createVariable(outNonMaterializedDocId);
    }
    for (auto& indVar : outNonMaterializedIndVars.second) {
      indVar.second = plan->getAst()->variables()->createVariable(indVar.second);
    }
  }

  auto c = std::make_unique<IndexNode>(plan, _id, _collection, outVariable, _indexes,
                                       std::unique_ptr<Condition>(_condition->clone()),
                                       _options);

  c->projections(_projections);
  c->needsGatherNodeSort(_needsGatherNodeSort);
  c->initIndexCoversProjections();
  c->_outNonMaterializedDocId = outNonMaterializedDocId;
  c->_outNonMaterializedIndVars = std::move(outNonMaterializedIndVars);
  CollectionAccessingNode::cloneInto(*c);
  DocumentProducingNode::cloneInto(plan, *c);
  return cloneHelper(std::move(c), withDependencies, withProperties);
}

/// @brief destroy the IndexNode
IndexNode::~IndexNode() = default;

/// @brief the cost of an index node is a multiple of the cost of
/// its unique dependency
CostEstimate IndexNode::estimateCost() const {
  CostEstimate estimate = _dependencies.at(0)->getCost();
  size_t incoming = estimate.estimatedNrItems;

  transaction::Methods* trx = _plan->getAst()->query()->trx();
  // estimate for the number of documents in the collection. may be outdated...
  size_t const itemsInCollection = _collection->count(trx);
  size_t totalItems = 0;
  double totalCost = 0.0;

  auto root = _condition->root();

  for (size_t i = 0; i < _indexes.size(); ++i) {
    Index::FilterCosts costs = Index::FilterCosts::defaultCosts(itemsInCollection);

    if (root != nullptr && root->numMembers() > i) {
      arangodb::aql::AstNode const* condition = root->getMember(i);
      costs = _indexes[i]->supportsFilterCondition(
          std::vector<std::shared_ptr<Index>>(), condition, _outVariable, itemsInCollection);
    }

    totalItems += costs.estimatedItems;
    totalCost += costs.estimatedCosts;
  }

  estimate.estimatedNrItems *= totalItems;
  estimate.estimatedCost += incoming * totalCost;
  return estimate;
}

/// @brief getVariablesUsedHere, modifying the set in-place
void IndexNode::getVariablesUsedHere(::arangodb::containers::HashSet<Variable const*>& vars) const {
  Ast::getReferencedVariables(_condition->root(), vars);

  vars.erase(_outVariable);
}
ExecutionNode::NodeType IndexNode::getType() const { return INDEX; }

Condition* IndexNode::condition() const { return _condition.get(); }

IndexIteratorOptions IndexNode::options() const { return _options; }

void IndexNode::setAscending(bool value) { _options.ascending = value; }

bool IndexNode::needsGatherNodeSort() const { return _needsGatherNodeSort; }

void IndexNode::needsGatherNodeSort(bool value) {
  _needsGatherNodeSort = value;
}

std::vector<Variable const*> IndexNode::getVariablesSetHere() const {
  if (!isLateMaterialized()) {
    return std::vector<Variable const*>{_outVariable};
  }

  std::vector<arangodb::aql::Variable const*> vars;
  vars.reserve(1 + _outNonMaterializedIndVars.second.size());
  vars.emplace_back(_outNonMaterializedDocId);
  std::transform(_outNonMaterializedIndVars.second.cbegin(),
                 _outNonMaterializedIndVars.second.cend(),
                 std::back_inserter(vars),
                 [](auto const& indVar) {
    return indVar.second;
  });

  return vars;
}

std::vector<transaction::Methods::IndexHandle> const& IndexNode::getIndexes() const {
  return _indexes;
}

void IndexNode::setLateMaterialized(aql::Variable const* docIdVariable,
                                    TRI_idx_iid_t commonIndexId,
                                    IndexVarsInfo const& indexVariables) {
  _outNonMaterializedIndVars.second.clear();
  _outNonMaterializedIndVars.first = commonIndexId;
  _outNonMaterializedDocId = docIdVariable;
  for (auto& indVars : indexVariables) {
    _outNonMaterializedIndVars.second[indVars.second.indexFieldNum] = indVars.second.var;
  }
}

NonConstExpression::NonConstExpression(std::unique_ptr<Expression> exp,
                                       std::vector<size_t>&& idxPath)
    : expression(std::move(exp)), indexPath(std::move(idxPath)) {}
