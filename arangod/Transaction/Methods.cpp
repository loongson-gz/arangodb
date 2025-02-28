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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Options.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

#include "Methods.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/Condition.h"
#include "Aql/SortCondition.h"
#include "Basics/AttributeNameParser.h"
#include "Basics/Exceptions.h"
#include "Basics/NumberUtils.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/encoding.h"
#include "Basics/system-compiler.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/ClusterTrxMethods.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/ReplicationTimeoutFeature.h"
#include "Cluster/ServerState.h"
#include "ClusterEngine/ClusterEngine.h"
#include "Containers/SmallVector.h"
#include "Futures/Utilities.h"
#include "Indexes/Index.h"
#include "Logger/Logger.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionCollection.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Context.h"
#include "Transaction/Helpers.h"
#include "Transaction/Options.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utils/OperationCursor.h"
#include "Utils/OperationOptions.h"
#include "VocBase/KeyLockInfo.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ManagedDocumentResult.h"
#include "VocBase/Methods/Indexes.h"
#include "VocBase/ticks.h"

using namespace arangodb;
using namespace arangodb::transaction;
using namespace arangodb::transaction::helpers;

template<typename T>
using Future = futures::Future<T>;

namespace {

enum class ReplicationType { NONE, LEADER, FOLLOWER };

// wrap vector inside a static function to ensure proper initialization order
std::vector<arangodb::transaction::Methods::DataSourceRegistrationCallback>& getDataSourceRegistrationCallbacks() {
  static std::vector<arangodb::transaction::Methods::DataSourceRegistrationCallback> callbacks;

  return callbacks;
}

/// @return the status change callbacks stored in state
///         or nullptr if none and !create
std::vector<arangodb::transaction::Methods::StatusChangeCallback const*>* getStatusChangeCallbacks(
    arangodb::TransactionState& state, bool create = false) {
  struct CookieType : public arangodb::TransactionState::Cookie {
    std::vector<arangodb::transaction::Methods::StatusChangeCallback const*> _callbacks;
  };

  static const int key = 0;  // arbitrary location in memory, common for all

// TODO FIXME find a better way to look up a ViewState
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto* cookie = dynamic_cast<CookieType*>(state.cookie(&key));
#else
  auto* cookie = static_cast<CookieType*>(state.cookie(&key));
#endif

  if (!cookie && create) {
    auto ptr = std::make_unique<CookieType>();

    cookie = ptr.get();
    state.cookie(&key, std::move(ptr));
  }

  return cookie ? &(cookie->_callbacks) : nullptr;
}

/// @brief notify callbacks of association of 'cid' with this TransactionState
/// @note done separately from addCollection() to avoid creating a
///       TransactionCollection instance for virtual entities, e.g. View
arangodb::Result applyDataSourceRegistrationCallbacks(LogicalDataSource& dataSource,
                                                      arangodb::transaction::Methods& trx) {
  for (auto& callback : getDataSourceRegistrationCallbacks()) {
    TRI_ASSERT(callback);  // addDataSourceRegistrationCallback(...) ensures valid

    try {
      auto res = callback(dataSource, trx);

      if (res.fail()) {
        return res;
      }
    } catch (...) {
      return arangodb::Result(TRI_ERROR_INTERNAL);
    }
  }

  return arangodb::Result();
}

/// @brief notify callbacks of association of 'cid' with this TransactionState
/// @note done separately from addCollection() to avoid creating a
///       TransactionCollection instance for virtual entities, e.g. View
void applyStatusChangeCallbacks(arangodb::transaction::Methods& trx,
                                arangodb::transaction::Status status) noexcept {
  TRI_ASSERT(arangodb::transaction::Status::ABORTED == status ||
             arangodb::transaction::Status::COMMITTED == status ||
             arangodb::transaction::Status::RUNNING == status);
  TRI_ASSERT(!trx.state()  // for embeded transactions status is not always updated
             || (trx.state()->isTopLevelTransaction() && trx.state()->status() == status) ||
             (!trx.state()->isTopLevelTransaction() &&
              arangodb::transaction::Status::RUNNING == trx.state()->status()));

  auto* state = trx.state();

  if (!state) {
    return;  // nothing to apply
  }

  auto* callbacks = getStatusChangeCallbacks(*state);

  if (!callbacks) {
    return;  // no callbacks to apply
  }

  // no need to lock since transactions are single-threaded
  for (auto& callback : *callbacks) {
    TRI_ASSERT(callback);  // addStatusChangeCallback(...) ensures valid

    try {
      (*callback)(trx, status);
    } catch (...) {
      // we must not propagate exceptions from here
    }
  }
}

static void throwCollectionNotFound(char const* name) {
  if (name == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                                 std::string(TRI_errno_string(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) +
                                     ": " + name);
}

/// @brief Insert an error reported instead of the new document
static void createBabiesError(VPackBuilder& builder,
                              std::unordered_map<int, size_t>& countErrorCodes,
                              Result const& error) {
  builder.openObject();
  builder.add(StaticStrings::Error, VPackValue(true));
  builder.add(StaticStrings::ErrorNum, VPackValue(error.errorNumber()));
  builder.add(StaticStrings::ErrorMessage, VPackValue(error.errorMessage()));
  builder.close();

  auto it = countErrorCodes.find(error.errorNumber());
  if (it == countErrorCodes.end()) {
    countErrorCodes.emplace(error.errorNumber(), 1);
  } else {
    it->second++;
  }
}

static OperationResult emptyResult(OperationOptions const& options) {
  VPackBuilder resultBuilder;
  resultBuilder.openArray();
  resultBuilder.close();
  return OperationResult(Result(), resultBuilder.steal(), options);
}
}  // namespace

/*static*/ void transaction::Methods::addDataSourceRegistrationCallback(
    DataSourceRegistrationCallback const& callback) {
  if (callback) {
    getDataSourceRegistrationCallbacks().emplace_back(callback);
  }
}

bool transaction::Methods::addStatusChangeCallback(StatusChangeCallback const* callback) {
  if (!callback || !*callback) {
    return true;  // nothing to call back
  } else if (!_state) {
    return false;  // nothing to add to
  }

  auto* statusChangeCallbacks = getStatusChangeCallbacks(*_state, true);

  TRI_ASSERT(nullptr != statusChangeCallbacks);  // 'create' was specified

  // no need to lock since transactions are single-threaded
  statusChangeCallbacks->emplace_back(callback);

  return true;
}

bool transaction::Methods::removeStatusChangeCallback(StatusChangeCallback const* callback) {
  if (!callback || !*callback) {
    return true;  // nothing to call back
  } else if (!_state) {
    return false;  // nothing to add to
  }

  auto* statusChangeCallbacks = getStatusChangeCallbacks(*_state, false);
  if (statusChangeCallbacks) {
    auto it = std::find(statusChangeCallbacks->begin(),
                        statusChangeCallbacks->end(), callback);
    TRI_ASSERT(it != statusChangeCallbacks->end());
    if (ADB_LIKELY(it != statusChangeCallbacks->end())) {
      statusChangeCallbacks->erase(it);
    }
  }
  return true;
}

/*static*/ void transaction::Methods::clearDataSourceRegistrationCallbacks() {
  getDataSourceRegistrationCallbacks().clear();
}

TRI_vocbase_t& transaction::Methods::vocbase() const {
  return _state->vocbase();
}

/// @brief whether or not the transaction consists of a single operation only
bool transaction::Methods::isSingleOperationTransaction() const {
  return _state->isSingleOperation();
}

/// @brief get the status of the transaction
transaction::Status transaction::Methods::status() const {
  return _state->status();
}

/// @brief sort ORs for the same attribute so they are in ascending value
/// order. this will only work if the condition is for a single attribute
/// the usedIndexes vector may also be re-sorted
bool transaction::Methods::sortOrs(arangodb::aql::Ast* ast, arangodb::aql::AstNode* root,
                                   arangodb::aql::Variable const* variable,
                                   std::vector<transaction::Methods::IndexHandle>& usedIndexes) {
  if (root == nullptr) {
    return true;
  }

  size_t const n = root->numMembers();

  if (n < 2) {
    return true;
  }

  if (n != usedIndexes.size()) {
    // sorting will break if the number of ORs is unequal to the number of
    // indexes but we shouldn't have got here then
    TRI_ASSERT(false);
    return false;
  }

  typedef std::pair<arangodb::aql::AstNode*, transaction::Methods::IndexHandle> ConditionData;
  ::arangodb::containers::SmallVector<ConditionData*>::allocator_type::arena_type a;
  ::arangodb::containers::SmallVector<ConditionData*> conditionData{a};

  auto cleanup = [&conditionData]() -> void {
    for (auto& it : conditionData) {
      delete it;
    }
  };

  TRI_DEFER(cleanup());

  std::vector<arangodb::aql::ConditionPart> parts;
  parts.reserve(n);

  std::pair<arangodb::aql::Variable const*, std::vector<arangodb::basics::AttributeName>> result;

  for (size_t i = 0; i < n; ++i) {
    // sort the conditions of each AND
    auto sub = root->getMemberUnchecked(i);

    TRI_ASSERT(sub != nullptr && sub->type == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_NARY_AND);
    size_t const nAnd = sub->numMembers();

    if (nAnd != 1) {
      // we can't handle this one
      return false;
    }

    auto operand = sub->getMemberUnchecked(0);

    if (!operand->isComparisonOperator()) {
      return false;
    }

    if (operand->type == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_NE ||
        operand->type == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_NIN) {
      return false;
    }

    auto lhs = operand->getMember(0);
    auto rhs = operand->getMember(1);

    if (lhs->type == arangodb::aql::AstNodeType::NODE_TYPE_ATTRIBUTE_ACCESS) {
      result.first = nullptr;
      result.second.clear();

      if (rhs->isConstant() && lhs->isAttributeAccessForVariable(result) &&
          result.first == variable &&
          (operand->type != arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_IN ||
           rhs->isArray())) {
        // create the condition data struct on the heap
        auto data = std::make_unique<ConditionData>(sub, usedIndexes[i]);
        // push it into an owning vector
        conditionData.emplace_back(data.get());
        // vector is now responsible for data
        auto p = data.release();
        // also add the pointer to the (non-owning) parts vector
        parts.emplace_back(result.first, result.second, operand,
                           arangodb::aql::AttributeSideType::ATTRIBUTE_LEFT, p);
      }
    }

    if (rhs->type == arangodb::aql::AstNodeType::NODE_TYPE_ATTRIBUTE_ACCESS ||
        rhs->type == arangodb::aql::AstNodeType::NODE_TYPE_EXPANSION) {
      result.first = nullptr;
      result.second.clear();

      if (lhs->isConstant() && rhs->isAttributeAccessForVariable(result) &&
          result.first == variable) {
        // create the condition data struct on the heap
        auto data = std::make_unique<ConditionData>(sub, usedIndexes[i]);
        // push it into an owning vector
        conditionData.emplace_back(data.get());
        // vector is now responsible for data
        auto p = data.release();
        // also add the pointer to the (non-owning) parts vector
        parts.emplace_back(result.first, result.second, operand,
                           arangodb::aql::AttributeSideType::ATTRIBUTE_RIGHT, p);
      }
    }
  }

  if (parts.size() != root->numMembers()) {
    return false;
  }

  // check if all parts use the same variable and attribute
  for (size_t i = 1; i < n; ++i) {
    auto const& lhs = parts[i - 1];
    auto const& rhs = parts[i];

    if (lhs.variable != rhs.variable || lhs.attributeName != rhs.attributeName) {
      // oops, the different OR parts are on different variables or attributes
      return false;
    }
  }

  size_t previousIn = SIZE_MAX;

  for (size_t i = 0; i < n; ++i) {
    auto& p = parts[i];

    if (p.operatorType == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_IN &&
        p.valueNode->isArray()) {
      TRI_ASSERT(p.valueNode->isConstant());

      if (previousIn != SIZE_MAX) {
        // merge IN with IN
        TRI_ASSERT(previousIn < i);
        auto emptyArray = ast->createNodeArray();
        auto mergedIn =
            ast->createNodeUnionizedArray(parts[previousIn].valueNode, p.valueNode);

        arangodb::aql::AstNode* clone = ast->clone(root->getMember(previousIn));
        root->changeMember(previousIn, clone);
        static_cast<ConditionData*>(parts[previousIn].data)->first = clone;

        clone = ast->clone(root->getMember(i));
        root->changeMember(i, clone);
        static_cast<ConditionData*>(parts[i].data)->first = clone;

        // can now edit nodes in place...
        parts[previousIn].valueNode = mergedIn;
        {
          auto n1 = root->getMember(previousIn)->getMember(0);
          TRI_ASSERT(n1->type == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_IN);
          TEMPORARILY_UNLOCK_NODE(n1);
          n1->changeMember(1, mergedIn);
        }

        p.valueNode = emptyArray;
        {
          auto n2 = root->getMember(i)->getMember(0);
          TRI_ASSERT(n2->type == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_IN);
          TEMPORARILY_UNLOCK_NODE(n2);
          n2->changeMember(1, emptyArray);
        }

      } else {
        // note first IN
        previousIn = i;
      }
    }
  }

  // now sort all conditions by variable name, attribute name, attribute value
  std::sort(parts.begin(), parts.end(),
            [](arangodb::aql::ConditionPart const& lhs,
               arangodb::aql::ConditionPart const& rhs) -> bool {
              // compare variable names first
              auto res = lhs.variable->name.compare(rhs.variable->name);

              if (res != 0) {
                return res < 0;
              }

              // compare attribute names next
              res = lhs.attributeName.compare(rhs.attributeName);

              if (res != 0) {
                return res < 0;
              }

              // compare attribute values next
              auto ll = lhs.lowerBound();
              auto lr = rhs.lowerBound();

              if (ll == nullptr && lr != nullptr) {
                // left lower bound is not set but right
                return true;
              } else if (ll != nullptr && lr == nullptr) {
                // left lower bound is set but not right
                return false;
              }

              if (ll != nullptr && lr != nullptr) {
                // both lower bounds are set
                res = CompareAstNodes(ll, lr, true);

                if (res != 0) {
                  return res < 0;
                }
              }

              if (lhs.isLowerInclusive() && !rhs.isLowerInclusive()) {
                return true;
              }
              if (rhs.isLowerInclusive() && !lhs.isLowerInclusive()) {
                return false;
              }

              // all things equal
              return false;
            });

  TRI_ASSERT(parts.size() == conditionData.size());

  // clean up
  while (root->numMembers()) {
    root->removeMemberUnchecked(0);
  }

  usedIndexes.clear();
  std::unordered_set<std::string> seenIndexConditions;

  // and rebuild
  for (size_t i = 0; i < n; ++i) {
    if (parts[i].operatorType == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_BINARY_IN &&
        parts[i].valueNode->isArray() && parts[i].valueNode->numMembers() == 0) {
      // can optimize away empty IN array
      continue;
    }

    auto conditionData = static_cast<ConditionData*>(parts[i].data);
    bool isUnique = true;

    if (!usedIndexes.empty()) {
      // try to find duplicate condition parts, and only return each
      // unique condition part once
      try {
        std::string conditionString =
            conditionData->first->toString() + " - " +
            std::to_string(conditionData->second->id());
        isUnique = seenIndexConditions.emplace(std::move(conditionString)).second;
        // we already saw the same combination of index & condition
        // don't add it again
      } catch (...) {
        // condition stringification may fail. in this case, we simply carry own
        // without simplifying the condition
      }
    }

    if (isUnique) {
      root->addMember(conditionData->first);
      usedIndexes.emplace_back(conditionData->second);
    }
  }

  return true;
}

std::pair<bool, bool> transaction::Methods::findIndexHandleForAndNode(
    std::vector<std::shared_ptr<Index>> const& indexes,
    arangodb::aql::AstNode* node, arangodb::aql::Variable const* reference,
    arangodb::aql::SortCondition const& sortCondition, size_t itemsInCollection,
    aql::IndexHint const& hint, std::vector<transaction::Methods::IndexHandle>& usedIndexes,
    arangodb::aql::AstNode*& specializedCondition, bool& isSparse) const {
  std::shared_ptr<Index> bestIndex;
  double bestCost = 0.0;
  bool bestSupportsFilter = false;
  bool bestSupportsSort = false;

  auto considerIndex = [&bestIndex, &bestCost, &bestSupportsFilter, &bestSupportsSort,
                        &indexes, node, reference, itemsInCollection,
                        &sortCondition](std::shared_ptr<Index> const& idx) -> void {
    TRI_ASSERT(!idx->inProgress());

    double filterCost = 0.0;
    double sortCost = 0.0;
    size_t itemsInIndex = itemsInCollection;
    size_t coveredAttributes = 0;

    bool supportsFilter = false;
    bool supportsSort = false;

    // check if the index supports the filter condition
    Index::FilterCosts costs =
        idx->supportsFilterCondition(indexes, node, reference, itemsInIndex);

    if (costs.supportsCondition) {
      // index supports the filter condition
      filterCost = costs.estimatedCosts;
      // this reduces the number of items left
      itemsInIndex = costs.estimatedItems;
      supportsFilter = true;
    } else {
      // index does not support the filter condition
      filterCost = itemsInIndex * 1.5;
    }

    bool const isOnlyAttributeAccess =
        (!sortCondition.isEmpty() && sortCondition.isOnlyAttributeAccess());

    if (sortCondition.isUnidirectional()) {
      // only go in here if we actually have a sort condition and it can in
      // general be supported by an index. for this, a sort condition must not
      // be empty, must consist only of attribute access, and all attributes
      // must be sorted in the direction
      Index::SortCosts costs =
          idx->supportsSortCondition(&sortCondition, reference, itemsInIndex);
      if (costs.supportsCondition) {
        supportsSort = true;
      }
      sortCost = costs.estimatedCosts;
      coveredAttributes = costs.coveredAttributes;
    }

    if (!supportsSort && isOnlyAttributeAccess && node->isOnlyEqualityMatch()) {
      // index cannot be used for sorting, but the filter condition consists
      // only of equality lookups (==)
      // now check if the index fields are the same as the sort condition fields
      // e.g. FILTER c.value1 == 1 && c.value2 == 42 SORT c.value1, c.value2
      if (coveredAttributes == sortCondition.numAttributes() &&
          (idx->isSorted() || idx->fields().size() == sortCondition.numAttributes())) {
        // no sorting needed
        sortCost = 0.0;
      }
    }

    if (!supportsFilter && !supportsSort) {
      return;
    }

    double totalCost = filterCost;
    if (!sortCondition.isEmpty()) {
      // only take into account the costs for sorting if there is actually
      // something to sort
      if (supportsSort) {
        totalCost += sortCost;
      } else {
        totalCost +=
            Index::SortCosts::defaultCosts(itemsInIndex, idx->isPersistent()).estimatedCosts;
      }
    }

    LOG_TOPIC("7278d", TRACE, Logger::FIXME)
        << "looking at index: " << idx.get()
        << ", isSorted: " << idx->isSorted()
        << ", isSparse: " << idx->sparse()
        << ", fields: " << idx->fields().size()
        << ", supportsFilter: " << supportsFilter
        << ", supportsSort: " << supportsSort
        << ", filterCost: " << (supportsFilter ? filterCost : 0.0)
        << ", sortCost: " << (supportsSort ? sortCost : 0.0)
        << ", totalCost: " << totalCost
        << ", isOnlyAttributeAccess: " << isOnlyAttributeAccess
        << ", isUnidirectional: " << sortCondition.isUnidirectional()
        << ", isOnlyEqualityMatch: " << node->isOnlyEqualityMatch()
        << ", itemsInIndex/estimatedItems: " << itemsInIndex;

    if (bestIndex == nullptr || totalCost < bestCost) {
      bestIndex = idx;
      bestCost = totalCost;
      bestSupportsFilter = supportsFilter;
      bestSupportsSort = supportsSort;
    }
  };

  if (hint.type() == aql::IndexHint::HintType::Simple) {
    std::vector<std::string> const& hintedIndices = hint.hint();
    for (std::string const& hinted : hintedIndices) {
      std::shared_ptr<Index> matched;
      for (std::shared_ptr<Index> const& idx : indexes) {
        if (idx->name() == hinted) {
          matched = idx;
          break;
        }
      }

      if (matched != nullptr) {
        considerIndex(matched);
        if (bestIndex != nullptr) {
          break;
        }
      }
    }

    if (hint.isForced() && bestIndex == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_FORCED_INDEX_HINT_UNUSABLE,
          "could not use index hint to serve query; " + hint.toString());
    }
  }

  if (bestIndex == nullptr) {
    for (auto const& idx : indexes) {
      considerIndex(idx);
    }
  }

  if (bestIndex == nullptr) {
    return std::make_pair(false, false);
  }

  specializedCondition = bestIndex->specializeCondition(node, reference);

  usedIndexes.emplace_back(bestIndex);
  isSparse = bestIndex->sparse();

  return std::make_pair(bestSupportsFilter, bestSupportsSort);
}

/// @brief Find out if any of the given requests has ended in a refusal
static bool findRefusal(std::vector<futures::Try<network::Response>> const& responses) {
  for (auto const& it : responses) {
    if (it.hasValue() && it.get().ok() &&
        it.get().response->statusCode() == fuerte::StatusNotAcceptable) {
      return true;
    }
  }
  return false;
}

transaction::Methods::Methods(std::shared_ptr<transaction::Context> const& transactionContext,
                              transaction::Options const& options)
    : _state(nullptr),
      _transactionContext(transactionContext),
      _transactionContextPtr(transactionContext.get()) {
  TRI_ASSERT(_transactionContextPtr != nullptr);

  // brief initialize the transaction
  // this will first check if the transaction is embedded in a parent
  // transaction. if not, it will create a transaction of its own
  // check in the context if we are running embedded
  TransactionState* parent = _transactionContextPtr->getParentTransaction();

  if (parent != nullptr) {  // yes, we are embedded
    if (!_transactionContextPtr->isEmbeddable()) {
      // we are embedded but this is disallowed...
      THROW_ARANGO_EXCEPTION(TRI_ERROR_TRANSACTION_NESTED);
    }

    _state = parent;
    TRI_ASSERT(_state != nullptr);
    _state->increaseNesting();
  } else {  // non-embedded
    // now start our own transaction
    StorageEngine* engine = EngineSelectorFeature::ENGINE;

    _state = engine
                 ->createTransactionState(_transactionContextPtr->vocbase(),
                                          _transactionContextPtr->generateId(), options)
                 .release();
    TRI_ASSERT(_state != nullptr && _state->isTopLevelTransaction());

    // register the transaction in the context
    _transactionContextPtr->registerTransaction(_state);
  }

  TRI_ASSERT(_state != nullptr);
}

/// @brief create the transaction, used to be UserTransaction
transaction::Methods::Methods(std::shared_ptr<transaction::Context> const& ctx,
                              std::vector<std::string> const& readCollections,
                              std::vector<std::string> const& writeCollections,
                              std::vector<std::string> const& exclusiveCollections,
                              transaction::Options const& options)
    : transaction::Methods(ctx, options) {
  addHint(transaction::Hints::Hint::LOCK_ENTIRELY);

  Result res;
  for (auto const& it : exclusiveCollections) {
    res = Methods::addCollection(it, AccessMode::Type::EXCLUSIVE);
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
  }
  for (auto const& it : writeCollections) {
    res = Methods::addCollection(it, AccessMode::Type::WRITE);
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
  }
  for (auto const& it : readCollections) {
    res = Methods::addCollection(it, AccessMode::Type::READ);
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
  }
}

/// @brief destroy the transaction
transaction::Methods::~Methods() {
  if (_state->isTopLevelTransaction()) {  // _nestingLevel == 0
    // unregister transaction from context
    _transactionContextPtr->unregisterTransaction();

    if (_state->status() == transaction::Status::RUNNING) {
      // auto abort a running transaction
      try {
        this->abort();
        TRI_ASSERT(_state->status() != transaction::Status::RUNNING);
      } catch (...) {
        // must never throw because we are in a dtor
      }
    }

    // free the state associated with the transaction
    TRI_ASSERT(_state->status() != transaction::Status::RUNNING);

    // store result in context
    _transactionContextPtr->storeTransactionResult(_state->id(),
                                                   _state->hasFailedOperations(),
                                                   _state->wasRegistered(),
                                                   _state->isReadOnlyTransaction());

    delete _state;
    _state = nullptr;
  } else {
    _state->decreaseNesting();  // return transaction
  }
}

/// @brief return the collection name resolver
CollectionNameResolver const* transaction::Methods::resolver() const {
  return &(_transactionContextPtr->resolver());
}

/// @brief return the transaction collection for a document collection
TransactionCollection* transaction::Methods::trxCollection(TRI_voc_cid_t cid,
                                                           AccessMode::Type type) const {
  TRI_ASSERT(_state != nullptr);
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING ||
             _state->status() == transaction::Status::CREATED);
  return _state->collection(cid, type);
}

/// @brief return the transaction collection for a document collection
TransactionCollection* transaction::Methods::trxCollection(std::string const& name,
                                                           AccessMode::Type type) const {
  TRI_ASSERT(_state != nullptr);
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING ||
             _state->status() == transaction::Status::CREATED);
  return _state->collection(name, type);
}

/// @brief order a ditch for a collection
void transaction::Methods::pinData(TRI_voc_cid_t cid) {
  TRI_ASSERT(_state != nullptr);
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING ||
             _state->status() == transaction::Status::CREATED);

  TransactionCollection* trxColl = trxCollection(cid, AccessMode::Type::READ);
  if (trxColl == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, "unable to determine transaction collection");
  }

  TRI_ASSERT(trxColl->collection() != nullptr);
  _transactionContextPtr->pinData(trxColl->collection().get());
}

/// @brief whether or not a ditch has been created for the collection
bool transaction::Methods::isPinned(TRI_voc_cid_t cid) const {
  return _transactionContextPtr->isPinned(cid);
}

/// @brief extract the _id attribute from a slice, and convert it into a
/// string
std::string transaction::Methods::extractIdString(VPackSlice slice) {
  return transaction::helpers::extractIdString(resolver(), slice, VPackSlice());
}

/// @brief build a VPack object with _id, _key and _rev, the result is
/// added to the builder in the argument as a single object.
void transaction::Methods::buildDocumentIdentity(
    LogicalCollection* collection, VPackBuilder& builder, TRI_voc_cid_t cid,
    arangodb::velocypack::StringRef const& key, TRI_voc_rid_t rid, TRI_voc_rid_t oldRid,
    ManagedDocumentResult const* oldDoc, ManagedDocumentResult const* newDoc) {
  StringLeaser leased(_transactionContextPtr);
  std::string& temp(*leased.get());
  temp.reserve(64);

  if (_state->isRunningInCluster()) {
    std::string resolved = resolver()->getCollectionNameCluster(cid);
#ifdef USE_ENTERPRISE
    if (resolved.compare(0, 7, "_local_") == 0) {
      resolved.erase(0, 7);
    } else if (resolved.compare(0, 6, "_from_") == 0) {
      resolved.erase(0, 6);
    } else if (resolved.compare(0, 4, "_to_") == 0) {
      resolved.erase(0, 4);
    }
#endif
    // build collection name
    temp.append(resolved);
  } else {
    // build collection name
    temp.append(collection->name());
  }

  // append / and key part
  temp.push_back('/');
  temp.append(key.data(), key.size());

  builder.openObject();
  builder.add(StaticStrings::IdString, VPackValue(temp));

  builder.add(StaticStrings::KeyString,
              VPackValuePair(key.data(), key.length(), VPackValueType::String));

  char ridBuffer[21];
  builder.add(StaticStrings::RevString, TRI_RidToValuePair(rid, &ridBuffer[0]));

  if (oldRid != 0) {
    builder.add("_oldRev", VPackValue(TRI_RidToString(oldRid)));
  }
  if (oldDoc != nullptr) {
    builder.add(VPackValue("old"));
    oldDoc->addToBuilder(builder, true);
  }
  if (newDoc != nullptr) {
    builder.add(VPackValue("new"));
    newDoc->addToBuilder(builder, true);
  }
  builder.close();
}

/// @brief begin the transaction
Result transaction::Methods::begin() {
  if (_state == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "invalid transaction state");
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  bool a = _localHints.has(transaction::Hints::Hint::FROM_TOPLEVEL_AQL);
  bool b = _localHints.has(transaction::Hints::Hint::GLOBAL_MANAGED);
  TRI_ASSERT(!(a && b));
#endif

  auto res = _state->beginTransaction(_localHints);
  if (res.fail()) {
    return res;
  }

  applyStatusChangeCallbacks(*this, Status::RUNNING);

  return Result();
}

/// @brief commit / finish the transaction
Future<Result> transaction::Methods::commitAsync() {
  TRI_IF_FAILURE("TransactionCommitFail") { return Result(TRI_ERROR_DEBUG); }

  if (_state == nullptr || _state->status() != transaction::Status::RUNNING) {
    // transaction not created or not running
    return Result(TRI_ERROR_TRANSACTION_INTERNAL,
                  "transaction not running on commit");
  }

  if (!_state->isReadOnlyTransaction()) {
    auto const& exec = ExecContext::current();
    bool cancelRW = ServerState::readOnly() && !exec.isSuperuser();
    if (exec.isCanceled() || cancelRW) {
      return Result(TRI_ERROR_ARANGO_READ_ONLY, "server is in read-only mode");
    }
  }

  auto f = futures::makeFuture(Result());
  if (_state->isRunningInCluster() && _state->isTopLevelTransaction()) {
    // first commit transaction on subordinate servers
    f = ClusterTrxMethods::commitTransaction(*this);
  }

  return std::move(f).thenValue([this](Result res) -> Result {
    if (res.fail()) {  // do not commit locally
      LOG_TOPIC("5743a", WARN, Logger::TRANSACTIONS)
          << "failed to commit on subordinates: '" << res.errorMessage() << "'";
      return res;
    }

    res = _state->commitTransaction(this);
    if (res.ok()) {
      applyStatusChangeCallbacks(*this, Status::COMMITTED);
    }

    return res;
  });
}

/// @brief abort the transaction
Future<Result> transaction::Methods::abortAsync() {
  if (_state == nullptr || _state->status() != transaction::Status::RUNNING) {
    // transaction not created or not running
    return Result(TRI_ERROR_TRANSACTION_INTERNAL,
                  "transaction not running on abort");
  }

  auto f = futures::makeFuture(Result());
  if (_state->isRunningInCluster() && _state->isTopLevelTransaction()) {
    // first commit transaction on subordinate servers
    f = ClusterTrxMethods::abortTransaction(*this);
  }

  return std::move(f).thenValue([this](Result res) -> Result {
    if (res.fail()) {  // do not commit locally
      LOG_TOPIC("d89a8", WARN, Logger::TRANSACTIONS)
          << "failed to abort on subordinates: " << res.errorMessage();
    }  // abort locally anyway

    res = _state->abortTransaction(this);
    if (res.ok()) {
      applyStatusChangeCallbacks(*this, Status::ABORTED);
    }

    return res;
  });
}

/// @brief finish a transaction (commit or abort), based on the previous state
Future<Result> transaction::Methods::finishAsync(Result const& res) {
  if (res.ok()) {
    // there was no previous error, so we'll commit
    return this->commitAsync();
  }

  // there was a previous error, so we'll abort
  return this->abortAsync().thenValue([res](Result ignore) {
    return res;  // return original error
  });
}

/// @brief return the transaction id
TRI_voc_tid_t transaction::Methods::tid() const {
  TRI_ASSERT(_state != nullptr);
  return _state->id();
}

std::string transaction::Methods::name(TRI_voc_cid_t cid) const {
  auto c = trxCollection(cid);
  if (c == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }
  return c->collectionName();
}

/// @brief read all master pointers, using skip and limit.
/// The resualt guarantees that all documents are contained exactly once
/// as long as the collection is not modified.
OperationResult transaction::Methods::any(std::string const& collectionName) {
  if (_state->isCoordinator()) {
    return anyCoordinator(collectionName);
  }
  return anyLocal(collectionName);
}

/// @brief fetches documents in a collection in random order, coordinator
OperationResult transaction::Methods::anyCoordinator(std::string const&) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

/// @brief fetches documents in a collection in random order, local
OperationResult transaction::Methods::anyLocal(std::string const& collectionName) {
  TRI_voc_cid_t cid = resolver()->getCollectionIdLocal(collectionName);

  if (cid == 0) {
    throwCollectionNotFound(collectionName.c_str());
  }

  pinData(cid);  // will throw when it fails

  VPackBuilder resultBuilder;
  resultBuilder.openArray();

  Result lockResult = lockRecursive(cid, AccessMode::Type::READ);

  if (!lockResult.ok() && !lockResult.is(TRI_ERROR_LOCKED)) {
    return OperationResult(lockResult);
  }

  OperationCursor cursor(indexScan(collectionName, transaction::Methods::CursorType::ANY));

  cursor.nextDocument(
      [&resultBuilder](LocalDocumentId const& token, VPackSlice slice) {
        resultBuilder.add(slice);
        return true;
      },
      1);

  if (lockResult.is(TRI_ERROR_LOCKED)) {
    Result res = unlockRecursive(cid, AccessMode::Type::READ);

    if (res.fail()) {
      return OperationResult(res);
    }
  }

  resultBuilder.close();

  return OperationResult(Result(), resultBuilder.steal());
}

TRI_voc_cid_t transaction::Methods::addCollectionAtRuntime(TRI_voc_cid_t cid,
                                                           std::string const& cname,
                                                           AccessMode::Type type) {
  auto collection = trxCollection(cid);

  if (collection == nullptr) {
    Result res = _state->addCollection(cid, cname, type, _state->nestingLevel(), true);

    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    auto dataSource = resolver()->getDataSource(cid);

    if (!dataSource) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    }

    res = applyDataSourceRegistrationCallbacks(*dataSource, *this);

    if (res.ok()) {
      res = _state->ensureCollections(_state->nestingLevel());
    }
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
    collection = trxCollection(cid);

    if (collection == nullptr) {
      throwCollectionNotFound(cname.c_str());
    }
  } else {
    AccessMode::Type collectionAccessType = collection->accessType();
    if (AccessMode::isRead(collectionAccessType) && !AccessMode::isRead(type)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION,
                                     std::string(TRI_errno_string(TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION)) + ": " + cname +
                                     " [" + AccessMode::typeString(type) + "]");
    }
  }

  TRI_ASSERT(collection != nullptr);
  return cid;
}

/// @brief add a collection to the transaction for read, at runtime
TRI_voc_cid_t transaction::Methods::addCollectionAtRuntime(std::string const& collectionName,
                                                           AccessMode::Type type) {
  if (collectionName == _collectionCache.name && !collectionName.empty()) {
    return _collectionCache.cid;
  }

  auto cid = resolver()->getCollectionIdLocal(collectionName);

  if (cid == 0) {
    throwCollectionNotFound(collectionName.c_str());
  }
  addCollectionAtRuntime(cid, collectionName, type);
  _collectionCache.cid = cid;
  _collectionCache.name = collectionName;
  return cid;
}

/// @brief return the type of a collection
bool transaction::Methods::isEdgeCollection(std::string const& collectionName) const {
  return getCollectionType(collectionName) == TRI_COL_TYPE_EDGE;
}

/// @brief return the type of a collection
bool transaction::Methods::isDocumentCollection(std::string const& collectionName) const {
  return getCollectionType(collectionName) == TRI_COL_TYPE_DOCUMENT;
}

/// @brief return the type of a collection
TRI_col_type_e transaction::Methods::getCollectionType(std::string const& collectionName) const {
  auto collection = resolver()->getCollection(collectionName);

  return collection ? collection->type() : TRI_COL_TYPE_UNKNOWN;
}

/// @brief return one document from a collection, fast path
///        If everything went well the result will contain the found document
///        (as an external on single_server) and this function will return
///        TRI_ERROR_NO_ERROR.
///        If there was an error the code is returned and it is guaranteed
///        that result remains unmodified.
///        Does not care for revision handling!
Result transaction::Methods::documentFastPath(std::string const& collectionName,
                                              ManagedDocumentResult* mmdr,
                                              VPackSlice const value,
                                              VPackBuilder& result, bool shouldLock) {
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);
  if (!value.isObject() && !value.isString()) {
    // must provide a document object or string
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  if (_state->isCoordinator()) {
    OperationOptions options;  // use default configuration

    OperationResult opRes = documentCoordinator(collectionName, value, options).get();
    if (opRes.fail()) {
      return opRes.result;
    }
    result.add(opRes.slice());
    return Result();
  }

  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::READ);
  auto const& collection = trxCollection(cid)->collection();

  pinData(cid);  // will throw when it fails

  arangodb::velocypack::StringRef key(transaction::helpers::extractKeyPart(value));
  if (key.empty()) {
    return Result(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
  }

  std::unique_ptr<ManagedDocumentResult> tmp;
  if (mmdr == nullptr) {
    tmp.reset(new ManagedDocumentResult);
    mmdr = tmp.get();
  }

  TRI_ASSERT(mmdr != nullptr);

  Result res =
      collection->read(this, key, *mmdr,
                       shouldLock && !isLocked(collection.get(), AccessMode::Type::READ));

  if (res.fail()) {
    return res;
  }

  TRI_ASSERT(isPinned(cid));

  mmdr->addToBuilder(result, true);
  return Result(TRI_ERROR_NO_ERROR);
}

/// @brief return one document from a collection, fast path
///        If everything went well the result will contain the found document
///        (as an external on single_server) and this function will return
///        TRI_ERROR_NO_ERROR.
///        If there was an error the code is returned
///        Does not care for revision handling!
///        Must only be called on a local server, not in cluster case!
Result transaction::Methods::documentFastPathLocal(std::string const& collectionName,
                                                   arangodb::velocypack::StringRef const& key,
                                                   ManagedDocumentResult& result,
                                                   bool shouldLock) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::READ);
  TransactionCollection* trxColl = trxCollection(cid);
  TRI_ASSERT(trxColl != nullptr);
  std::shared_ptr<LogicalCollection> const& collection = trxColl->collection();
  TRI_ASSERT(collection != nullptr);
  _transactionContextPtr->pinData(collection.get());  // will throw when it fails

  if (key.empty()) {
    return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
  }

  bool isLocked = trxColl->isLocked(AccessMode::Type::READ, _state->nestingLevel());
  Result res = collection->read(this, key, result, shouldLock && !isLocked);
  TRI_ASSERT(res.fail() || isPinned(cid));
  return res;
}

namespace {
template<typename F>
Future<OperationResult> addTracking(Future<OperationResult> f,
                                    VPackSlice value,
                                    F&& func) {
#ifdef USE_ENTERPRISE
  return std::move(f).thenValue([func = std::forward<F>(func), value](OperationResult opRes) {
    func(opRes, value);
    return opRes;
  });
#else
  return f;
#endif
}
}

/// @brief return one or multiple documents from a collection
Future<OperationResult> transaction::Methods::documentAsync(std::string const& cname,
                                               VPackSlice const value,
                                               OperationOptions& options) {
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  if (!value.isObject() && !value.isArray()) {
    // must provide a document object or an array of documents
    events::ReadDocument(vocbase().name(), cname, value, options,
                         TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }

  OperationResult result;
  if (_state->isCoordinator()) {
    return addTracking(documentCoordinator(cname, value, options), value,
                       [=](OperationResult const& opRes, VPackSlice data) {
      events::ReadDocument(vocbase().name(), cname, data, opRes._options, opRes.errorNumber());
    });
  } else {
    return documentLocal(cname, value, options);
  }
}

/// @brief read one or multiple documents in a collection, coordinator
#ifndef USE_ENTERPRISE
Future<OperationResult> transaction::Methods::documentCoordinator(
    std::string const& collectionName, VPackSlice const value, OperationOptions& options) {
  if (!value.isArray()) {
    arangodb::velocypack::StringRef key(transaction::helpers::extractKeyPart(value));
    if (key.empty()) {
      return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD);
    }
  }

  ClusterInfo& ci = vocbase().server().getFeature<ClusterFeature>().clusterInfo();
  auto colptr = ci.getCollectionNT(vocbase().name(), collectionName);
  if (colptr == nullptr) {
    return futures::makeFuture(OperationResult(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND));
  }

  return arangodb::getDocumentOnCoordinator(*this, *colptr, value, options);
}
#endif

/// @brief read one or multiple documents in a collection, local
Future<OperationResult> transaction::Methods::documentLocal(std::string const& collectionName,
                                                            VPackSlice const value,
                                                            OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::READ);
  std::shared_ptr<LogicalCollection> const& collection = trxCollection(cid)->collection();

  if (!options.silent) {
    pinData(cid);  // will throw when it fails
  }

  VPackBuilder resultBuilder;
  ManagedDocumentResult result;

  auto workForOneDocument = [&](VPackSlice const value, bool isMultiple) -> Result {
    arangodb::velocypack::StringRef key(transaction::helpers::extractKeyPart(value));
    if (key.empty()) {
      return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
    }

    TRI_voc_rid_t expectedRevision = 0;
    if (!options.ignoreRevs && value.isObject()) {
      expectedRevision = TRI_ExtractRevisionId(value);
    }

    result.clear();

    Result res = collection->read(this, key, result,
                                  !isLocked(collection.get(), AccessMode::Type::READ));

    if (res.fail()) {
      return res;
    }

    TRI_ASSERT(isPinned(cid));

    if (expectedRevision != 0) {
      TRI_voc_rid_t foundRevision =
          transaction::helpers::extractRevFromDocument(VPackSlice(result.vpack()));
      if (expectedRevision != foundRevision) {
        if (!isMultiple) {
          // still return
          buildDocumentIdentity(collection.get(), resultBuilder, cid, key,
                                foundRevision, 0, nullptr, nullptr);
        }
        return TRI_ERROR_ARANGO_CONFLICT;
      }
    }

    if (!options.silent) {
      result.addToBuilder(resultBuilder, true);
    } else if (isMultiple) {
      resultBuilder.add(VPackSlice::nullSlice());
    }

    return TRI_ERROR_NO_ERROR;
  };

  Result res;
  std::unordered_map<int, size_t> countErrorCodes;
  if (!value.isArray()) {
    res = workForOneDocument(value, false);
  } else {
    VPackArrayBuilder guard(&resultBuilder);
    for (VPackSlice s : VPackArrayIterator(value)) {
      res = workForOneDocument(s, true);
      if (res.fail()) {
        createBabiesError(resultBuilder, countErrorCodes, res);
      }
    }
    res.reset(); // With babies the reporting is handled somewhere else.
  }

  events::ReadDocument(vocbase().name(), collectionName, value, options, res.errorNumber());

  return futures::makeFuture(OperationResult(std::move(res), resultBuilder.steal(),
                                             options, countErrorCodes));
}

/// @brief create one or multiple documents in a collection
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
Future<OperationResult> transaction::Methods::insertAsync(std::string const& cname,
                                                          VPackSlice const value,
                                                          OperationOptions const& options) {
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  if (!value.isObject() && !value.isArray()) {
    // must provide a document object or an array of documents
    events::CreateDocument(vocbase().name(), cname, value, options,
                           TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }
  if (value.isArray() && value.length() == 0) {
    events::CreateDocument(vocbase().name(), cname, value, options, TRI_ERROR_NO_ERROR);
    return emptyResult(options);
  }

  auto f = Future<OperationResult>::makeEmpty();
  if (_state->isCoordinator()) {
    f = insertCoordinator(cname, value, options);
  } else {
    OperationOptions optionsCopy = options;
    f = insertLocal(cname, value, optionsCopy);
  }

  return addTracking(std::move(f), value,
                     [=](OperationResult const& opRes, VPackSlice data) {
                       events::CreateDocument(vocbase().name(), cname,
                                              (opRes.ok() && opRes._options.returnNew) ? opRes.slice() : data,
                                              opRes._options, opRes.errorNumber());
                     });
}

/// @brief create one or multiple documents in a collection, coordinator
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
#ifndef USE_ENTERPRISE
Future<OperationResult> transaction::Methods::insertCoordinator(std::string const& collectionName,
                                                                VPackSlice const value,
                                                                OperationOptions const& options) {
  auto& ci = vocbase().server().getFeature<ClusterFeature>().clusterInfo();
  auto colptr = ci.getCollectionNT(vocbase().name(), collectionName);
  if (colptr == nullptr) {
    return futures::makeFuture(OperationResult(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND));
  }
  return arangodb::createDocumentOnCoordinator(*this, *colptr, value, options);
}
#endif

/// @brief choose a timeout for synchronous replication, based on the
/// number of documents we ship over
static double chooseTimeout(size_t count, size_t totalBytes) {
  // We usually assume that a server can process at least 2500 documents
  // per second (this is a low estimate), and use a low limit of 0.5s
  // and a high timeout of 120s
  double timeout = count / 2500.0;

  // Really big documents need additional adjustment. Using total size
  // of all messages to handle worst case scenario of constrained resource
  // processing all
  timeout += (totalBytes / 4096) * ReplicationTimeoutFeature::timeoutPer4k;

  if (timeout < ReplicationTimeoutFeature::lowerLimit) {
    return ReplicationTimeoutFeature::lowerLimit * ReplicationTimeoutFeature::timeoutFactor;
  }
  return (std::min)(120.0, timeout) * ReplicationTimeoutFeature::timeoutFactor;
}

/// @brief create one or multiple documents in a collection, local
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
Future<OperationResult> transaction::Methods::insertLocal(std::string const& cname,
                                                          VPackSlice const value,
                                                          OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(cname, AccessMode::Type::WRITE);
  std::shared_ptr<LogicalCollection> const& collection = trxCollection(cid)->collection();

  bool const needsLock = !isLocked(collection.get(), AccessMode::Type::WRITE);

  // If we maybe will overwrite, we cannot do single document operations, thus:
  // options.overwrite => !needsLock
  TRI_ASSERT(!options.overwrite || !needsLock);

  bool const isMMFiles = EngineSelectorFeature::isMMFiles();

  // Assert my assumption that we don't have a lock only with mmfiles single
  // document operations.

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  {
    bool const isMock = EngineSelectorFeature::ENGINE->typeName() == "Mock";
    if (!isMock) {
      // needsLock => isMMFiles
      // needsLock => !value.isArray()
      // needsLock => _localHints.has(Hints::Hint::SINGLE_OPERATION))
      // However, due to nested transactions, there are mmfiles single
      // operations that already have a lock.
      TRI_ASSERT(!needsLock || isMMFiles);
      TRI_ASSERT(!needsLock || !value.isArray());
      TRI_ASSERT(!needsLock || _localHints.has(Hints::Hint::SINGLE_OPERATION));
    }
  }
#endif

  // If we are
  // - not on a single server (i.e. maybe replicating),
  // - using the MMFiles storage engine, and
  // - doing a single document operation,
  // we have to:
  // - Get the list of followers during the time span we actually do hold a
  //   collection level lock. This is to avoid races with the replication where
  //   a follower may otherwise be added between the actual document operation
  //   and the point where we get our copy of the followers, regardless of the
  //   latter happens before or after the document operation.

  // Note that getting the followers this way also doesn't do any harm in other
  // cases, except for babies because it would be done multiple times. Thus this
  // bool.
  // I suppose alternatively we could also do it via the updateFollowers
  // callback and set updateFollowers to nullptr afterwards, so we only do it
  // once.
  bool const needsToGetFollowersUnderLock = needsLock && _state->isDBServer();

  std::shared_ptr<std::vector<ServerID> const> followers;

  std::function<void()> updateFollowers;

  if (needsToGetFollowersUnderLock) {
    FollowerInfo const& followerInfo = *collection->followers();

    updateFollowers = [&followerInfo, &followers]() {
      TRI_ASSERT(followers == nullptr);
      followers = followerInfo.get();
    };
  } else if (_state->isDBServer()) {
    TRI_ASSERT(followers == nullptr);
    followers = collection->followers()->get();
  }

  // we may need to lock individual keys here so we can ensure that even with
  // concurrent operations on the same keys we have the same order of data
  // application on leader and followers
  KeyLockInfo keyLockInfo;

  ReplicationType replicationType = ReplicationType::NONE;
  if (_state->isDBServer()) {
    // Block operation early if we are not supposed to perform it:
    auto const& followerInfo = collection->followers();
    std::string theLeader = followerInfo->getLeader();
    if (theLeader.empty()) {
      if (!options.isSynchronousReplicationFrom.empty()) {
        return OperationResult(TRI_ERROR_CLUSTER_SHARD_LEADER_REFUSES_REPLICATION, options);
      }
      if (!followerInfo->allowedToWrite()) {
        // We cannot fulfill minimum replication Factor.
        // Reject write.
        LOG_TOPIC("d7306", ERR, Logger::REPLICATION)
            << "Less than minReplicationFactor ("
            << basics::StringUtils::itoa(collection->writeConcern())
            << ") followers in sync. Shard  " << collection->name()
            << " is temporarily in read-only mode.";
        return OperationResult(TRI_ERROR_ARANGO_READ_ONLY, options);
      }

      replicationType = ReplicationType::LEADER;
      if (isMMFiles && needsLock) {
        keyLockInfo.shouldLock = true;
      }
      // We cannot be silent if we may have to replicate later.
      // If we need to get the followers under the single document operation's
      // lock, we don't know yet if we will have followers later and thus cannot
      // be silent.
      // Otherwise, if we already know the followers to replicate to, we can
      // just check if they're empty.
      if (needsToGetFollowersUnderLock || keyLockInfo.shouldLock || !followers->empty()) {
        options.silent = false;
      }
    } else {  // we are a follower following theLeader
      replicationType = ReplicationType::FOLLOWER;
      if (options.isSynchronousReplicationFrom.empty()) {
        return OperationResult(TRI_ERROR_CLUSTER_SHARD_LEADER_RESIGNED, options);
      }
      if (options.isSynchronousReplicationFrom != theLeader) {
        return OperationResult(TRI_ERROR_CLUSTER_SHARD_FOLLOWER_REFUSES_OPERATION, options);
      }
    }
  }  // isDBServer - early block
  if (options.returnOld || options.returnNew) {
    pinData(cid);  // will throw when it fails
  }

  VPackBuilder resultBuilder;
  ManagedDocumentResult docResult;
  ManagedDocumentResult prevDocResult;  // return OLD (with override option)

  auto workForOneDocument = [&](VPackSlice const value, bool isBabies) -> Result {
    if (!value.isObject()) {
      return Result(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    }

    int r = validateSmartJoinAttribute(*collection, value);

    if (r != TRI_ERROR_NO_ERROR) {
      return Result(r);
    }

    docResult.clear();
    prevDocResult.clear();

    // insert with overwrite may NOT be a single document operation, as we
    // possibly need to do two separate operations (insert and replace).
    TRI_ASSERT(!(options.overwrite && needsLock));

    TRI_ASSERT(needsLock == !isLocked(collection.get(), AccessMode::Type::WRITE));
    Result res = collection->insert(this, value, docResult, options, needsLock,
                                    &keyLockInfo, updateFollowers);

    bool didReplace = false;
    if (options.overwrite && res.is(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED)) {
      // RepSert Case - unique_constraint violated ->  try replace
      // If we're overwriting, we already have a lock. Therefore we also don't
      // need to get the followers under the lock.
      TRI_ASSERT(!needsLock);
      TRI_ASSERT(!needsToGetFollowersUnderLock);
      TRI_ASSERT(updateFollowers == nullptr);
      res = collection->replace(this, value, docResult, options,
                                /*lock*/ false, prevDocResult);
      TRI_ASSERT(res.fail() || prevDocResult.revisionId() != 0);
      didReplace = true;
    }

    if (res.fail()) {
      // Error reporting in the babies case is done outside of here,
      if (res.is(TRI_ERROR_ARANGO_CONFLICT) && !isBabies && prevDocResult.revisionId() != 0) {
        TRI_ASSERT(didReplace);
        
        arangodb::velocypack::StringRef key = value.get(StaticStrings::KeyString).stringRef();
        buildDocumentIdentity(collection.get(), resultBuilder, cid, key, prevDocResult.revisionId(),
                              0, nullptr, nullptr);
      }
      return res;
    }

    if (!options.silent) {
      bool const showReplaced = (options.returnOld && didReplace);
      TRI_ASSERT(!options.returnNew || !docResult.empty());
      TRI_ASSERT(!showReplaced || !prevDocResult.empty());

      arangodb::velocypack::StringRef keyString;
      if (didReplace) {  // docResult may be empty, but replace requires '_key' in value
        keyString = value.get(StaticStrings::KeyString);
        TRI_ASSERT(!keyString.empty());
      } else {
        keyString =
            transaction::helpers::extractKeyFromDocument(VPackSlice(docResult.vpack()));
      }

      buildDocumentIdentity(collection.get(), resultBuilder, cid, keyString,
                            docResult.revisionId(), prevDocResult.revisionId(),
                            showReplaced ? &prevDocResult : nullptr,
                            options.returnNew ? &docResult : nullptr);
    }
    return Result();
  };

  Result res;
  std::unordered_map<int, size_t> errorCounter;
  if (value.isArray()) {
    VPackArrayBuilder b(&resultBuilder);
    for (VPackSlice s : VPackArrayIterator(value)) {
      res = workForOneDocument(s, true);
      if (res.fail()) {
        createBabiesError(resultBuilder, errorCounter, res);
      }
    }
    res.reset(); // With babies reporting is handled in the result body
  } else {
    res = workForOneDocument(value, false);
  }

  auto resDocs = resultBuilder.steal();
  if (res.ok() && replicationType == ReplicationType::LEADER) {
    TRI_ASSERT(collection != nullptr);
    TRI_ASSERT(followers != nullptr);

    // In the multi babies case res is always TRI_ERROR_NO_ERROR if we
    // get here, in the single document case, we do not try to replicate
    // in case of an error.

    // Now replicate the good operations on all followers:
    return replicateOperations(collection.get(), followers, options, value,
                               TRI_VOC_DOCUMENT_OPERATION_INSERT, resDocs)
    .thenValue([options, errs = std::move(errorCounter), resDocs](Result res) {
      if (!res.ok()) {
        return OperationResult{std::move(res), options};
      }
      if (options.silent && errs.empty()) {
        // We needed the results, but do not want to report:
        resDocs->clear();
      }
      return OperationResult(std::move(res), std::move(resDocs), options, std::move(errs));
    });
  }
  if (options.silent && errorCounter.empty()) {
    // We needed the results, but do not want to report:
    resDocs->clear();
  }
  return futures::makeFuture(OperationResult(std::move(res), std::move(resDocs),
                                             options, std::move(errorCounter)));
}

/// @brief update/patch one or multiple documents in a collection
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
Future<OperationResult> transaction::Methods::updateAsync(std::string const& cname,
                                                          VPackSlice const newValue,
                                                          OperationOptions const& options) {
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  if (!newValue.isObject() && !newValue.isArray()) {
    // must provide a document object or an array of documents
    events::ModifyDocument(vocbase().name(), cname, newValue, options,
                           TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }
  if (newValue.isArray() && newValue.length() == 0) {
    events::ModifyDocument(vocbase().name(), cname, newValue, options,
                           TRI_ERROR_NO_ERROR);
    return emptyResult(options);
  }

  auto f = Future<OperationResult>::makeEmpty();
  if (_state->isCoordinator()) {
    f = modifyCoordinator(cname, newValue, options, TRI_VOC_DOCUMENT_OPERATION_UPDATE);
  } else {
    OperationOptions optionsCopy = options;
    f = modifyLocal(cname, newValue, optionsCopy, TRI_VOC_DOCUMENT_OPERATION_UPDATE);
  }
  return addTracking(std::move(f), newValue, [=](OperationResult const& opRes,
                                                 VPackSlice data) {
    events::ModifyDocument(vocbase().name(), cname, data,
                           opRes._options, opRes.errorNumber());
  });
}

/// @brief update one or multiple documents in a collection, coordinator
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
#ifndef USE_ENTERPRISE
Future<OperationResult> transaction::Methods::modifyCoordinator(
    std::string const& cname, VPackSlice const newValue,
    OperationOptions const& options, TRI_voc_document_operation_e operation) {
  if (!newValue.isArray()) {
    arangodb::velocypack::StringRef key(transaction::helpers::extractKeyPart(newValue));
    if (key.empty()) {
      return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD);
    }
  }

  ClusterInfo& ci = vocbase().server().getFeature<ClusterFeature>().clusterInfo();
  auto colptr = ci.getCollectionNT(vocbase().name(), cname);
  if (colptr == nullptr) {
    return futures::makeFuture(OperationResult(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND));
  }

  const bool isPatch = (TRI_VOC_DOCUMENT_OPERATION_UPDATE == operation);
  return arangodb::modifyDocumentOnCoordinator(*this, *colptr, newValue, options, isPatch);
}
#endif

/// @brief replace one or multiple documents in a collection
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
Future<OperationResult> transaction::Methods::replaceAsync(std::string const& cname,
                                              VPackSlice const newValue,
                                              OperationOptions const& options) {
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  if (!newValue.isObject() && !newValue.isArray()) {
    // must provide a document object or an array of documents
    events::ReplaceDocument(vocbase().name(), cname, newValue, options,
                            TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }
  if (newValue.isArray() && newValue.length() == 0) {
    events::ReplaceDocument(vocbase().name(), cname, newValue, options,
                            TRI_ERROR_NO_ERROR);
    return futures::makeFuture(emptyResult(options));
  }

  auto f = Future<OperationResult>::makeEmpty();
  if (_state->isCoordinator()) {
    f = modifyCoordinator(cname, newValue, options, TRI_VOC_DOCUMENT_OPERATION_REPLACE);
  } else {
    OperationOptions optionsCopy = options;
    f = modifyLocal(cname, newValue, optionsCopy, TRI_VOC_DOCUMENT_OPERATION_REPLACE);
  }
  return addTracking(std::move(f), newValue, [=](OperationResult const& opRes,
                                                 VPackSlice data) {
    events::ReplaceDocument(vocbase().name(), cname, data,
                           opRes._options, opRes.errorNumber());
  });
}

/// @brief replace one or multiple documents in a collection, local
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
Future<OperationResult> transaction::Methods::modifyLocal(std::string const& collectionName,
                                                  VPackSlice const newValue,
                                                  OperationOptions& options,
                                                  TRI_voc_document_operation_e operation) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::WRITE);
  auto const& collection = trxCollection(cid)->collection();

  bool const needsLock = !isLocked(collection.get(), AccessMode::Type::WRITE);

  // Assert my assumption that we don't have a lock only with mmfiles single
  // document operations.

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  {
    bool const isMMFiles = EngineSelectorFeature::isMMFiles();
    bool const isMock = EngineSelectorFeature::ENGINE->typeName() == "Mock";
    if (!isMock) {
      // needsLock => isMMFiles
      // needsLock => !newValue.isArray()
      // needsLock => _localHints.has(Hints::Hint::SINGLE_OPERATION))
      // However, due to nested transactions, there are mmfiles single
      // operations that already have a lock.
      TRI_ASSERT(!needsLock || isMMFiles);
      TRI_ASSERT(!needsLock || !newValue.isArray());
      TRI_ASSERT(!needsLock || _localHints.has(Hints::Hint::SINGLE_OPERATION));
    }
  }
#endif

  // If we are
  // - not on a single server (i.e. maybe replicating),
  // - using the MMFiles storage engine, and
  // - doing a single document operation,
  // we have to:
  // - Get the list of followers during the time span we actually do hold a
  //   collection level lock. This is to avoid races with the replication where
  //   a follower may otherwise be added between the actual document operation
  //   and the point where we get our copy of the followers, regardless of the
  //   latter happens before or after the document operation.
  // In update/replace we do NOT have to get document level locks as in insert
  // or remove, as we still hold a lock during the replication in this case.
  bool const needsToGetFollowersUnderLock = needsLock && _state->isDBServer();

  std::shared_ptr<std::vector<ServerID> const> followers;

  if (_state->isDBServer()) {
    TRI_ASSERT(followers == nullptr);
    followers = collection->followers()->get();
  }

  ReplicationType replicationType = ReplicationType::NONE;
  if (_state->isDBServer()) {
    // Block operation early if we are not supposed to perform it:
    auto const& followerInfo = collection->followers();
    std::string theLeader = followerInfo->getLeader();
    if (theLeader.empty()) {
      if (!options.isSynchronousReplicationFrom.empty()) {
        return OperationResult(TRI_ERROR_CLUSTER_SHARD_LEADER_REFUSES_REPLICATION);
      }
      if (!followerInfo->allowedToWrite()) {
        // We cannot fulfill minimum replication Factor.
        // Reject write.
        LOG_TOPIC("2e35a", ERR, Logger::REPLICATION)
            << "Less than minReplicationFactor ("
            << basics::StringUtils::itoa(collection->writeConcern())
            << ") followers in sync. Shard  " << collection->name()
            << " is temporarily in read-only mode.";
        return OperationResult(TRI_ERROR_ARANGO_READ_ONLY, options);
      }

      replicationType = ReplicationType::LEADER;
      // We cannot be silent if we may have to replicate later.
      // If we need to get the followers under the single document operation's
      // lock, we don't know yet if we will have followers later and thus cannot
      // be silent.
      // Otherwise, if we already know the followers to replicate to, we can
      // just check if they're empty.
      if (needsToGetFollowersUnderLock || !followers->empty()) {
        options.silent = false;
      }
    } else {  // we are a follower following theLeader
      replicationType = ReplicationType::FOLLOWER;
      if (options.isSynchronousReplicationFrom.empty()) {
        return OperationResult(TRI_ERROR_CLUSTER_SHARD_LEADER_RESIGNED);
      }
      if (options.isSynchronousReplicationFrom != theLeader) {
        return OperationResult(TRI_ERROR_CLUSTER_SHARD_FOLLOWER_REFUSES_OPERATION);
      }
    }
  }  // isDBServer - early block

  if (options.returnOld || options.returnNew) {
    pinData(cid);  // will throw when it fails
  }

  // Update/replace are a read and a write, let's get the write lock already
  // for the read operation:
  Result lockResult = lockRecursive(cid, AccessMode::Type::WRITE);

  if (!lockResult.ok() && !lockResult.is(TRI_ERROR_LOCKED)) {
    return OperationResult(lockResult);
  }
  // Iff we didn't have a lock before, we got one now.
  TRI_ASSERT(needsLock == lockResult.is(TRI_ERROR_LOCKED));

  VPackBuilder resultBuilder;  // building the complete result
  ManagedDocumentResult previous;
  ManagedDocumentResult result;

  // lambda //////////////
  auto workForOneDocument = [this, &operation, &options, &collection,
                             &resultBuilder, &cid, &previous,
                             &result](VPackSlice const newVal, bool isBabies) -> Result {
    Result res;
    if (!newVal.isObject()) {
      res.reset(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
      return res;
    }

    result.clear();
    previous.clear();

    // replace and update are two operations each, thus this can and must not be
    // single document operations. We need to have a lock here already.
    TRI_ASSERT(isLocked(collection.get(), AccessMode::Type::WRITE));

    if (operation == TRI_VOC_DOCUMENT_OPERATION_REPLACE) {
      res = collection->replace(this, newVal, result, options,
                                /*lock*/ false, previous);
    } else {
      res = collection->update(this, newVal, result, options,
                               /*lock*/ false, previous);
    }

    if (res.fail()) {
      if (res.is(TRI_ERROR_ARANGO_CONFLICT) && !isBabies) {
        TRI_ASSERT(previous.revisionId() != 0);
        arangodb::velocypack::StringRef key(newVal.get(StaticStrings::KeyString));
        buildDocumentIdentity(collection.get(), resultBuilder, cid, key,
                              previous.revisionId(), 0,
                              options.returnOld ? &previous : nullptr, nullptr);
      }
      return res;
    }

    if (!options.silent) {
      TRI_ASSERT(!options.returnOld || !previous.empty());
      TRI_ASSERT(!options.returnNew || !result.empty());
      TRI_ASSERT(result.revisionId() != 0 && previous.revisionId() != 0);

      arangodb::velocypack::StringRef key(newVal.get(StaticStrings::KeyString));
      buildDocumentIdentity(collection.get(), resultBuilder, cid, key,
                            result.revisionId(), previous.revisionId(),
                            options.returnOld ? &previous : nullptr,
                            options.returnNew ? &result : nullptr);
    }

    return res;  // must be ok!
  };             // workForOneDocument
  ///////////////////////

  bool multiCase = newValue.isArray();
  std::unordered_map<int, size_t> errorCounter;
  Result res;
  if (multiCase) {
    {
      VPackArrayBuilder guard(&resultBuilder);
      VPackArrayIterator it(newValue);
      while (it.valid()) {
        res = workForOneDocument(it.value(), true);
        if (res.fail()) {
          createBabiesError(resultBuilder, errorCounter, res);
        }
        ++it;
      }
    }
    res.reset(); // With babies reporting is handled in the result body
  } else {
    res = workForOneDocument(newValue, false);
  }

  auto resDocs = resultBuilder.steal();
  if (res.ok() && replicationType == ReplicationType::LEADER) {
    // We still hold a lock here, because this is update/replace and we're
    // therefore not doing single document operations. But if we didn't hold it
    // at the beginning of the method the followers may not be up-to-date.
    TRI_ASSERT(isLocked(collection.get(), AccessMode::Type::WRITE));
    if (needsToGetFollowersUnderLock) {
      followers = collection->followers()->get();
    }

    TRI_ASSERT(collection != nullptr);
    TRI_ASSERT(followers != nullptr);

    // In the multi babies case res is always TRI_ERROR_NO_ERROR if we
    // get here, in the single document case, we do not try to replicate
    // in case of an error.

    // Now replicate the good operations on all followers:
    return replicateOperations(collection.get(), followers, options, newValue,
                               operation, resDocs)
    .thenValue([options, errs = std::move(errorCounter), resDocs](Result&& res) {
      if (!res.ok()) {
        return OperationResult{std::move(res), options};
      }
      if (options.silent && errs.empty()) {
        // We needed the results, but do not want to report:
        resDocs->clear();
      }
      return OperationResult(std::move(res), std::move(resDocs),
                             std::move(options), std::move(errs));
    });
  }

  if (options.silent && errorCounter.empty()) {
    // We needed the results, but do not want to report:
    resDocs->clear();
  }

  return OperationResult(std::move(res), std::move(resDocs),
                         std::move(options), std::move(errorCounter));
}

/// @brief remove one or multiple documents in a collection
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
Future<OperationResult> transaction::Methods::removeAsync(std::string const& cname,
                                                  VPackSlice const value,
                                                  OperationOptions const& options) {
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  if (!value.isObject() && !value.isArray() && !value.isString()) {
    // must provide a document object or an array of documents
    events::DeleteDocument(vocbase().name(), cname, value, options,
                           TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
  }
  if (value.isArray() && value.length() == 0) {
    events::DeleteDocument(vocbase().name(), cname, value, options, TRI_ERROR_NO_ERROR);
    return emptyResult(options);
  }

  auto f = Future<OperationResult>::makeEmpty();
  if (_state->isCoordinator()) {
    f = removeCoordinator(cname, value, options);
  } else {
    OperationOptions optionsCopy = options;
    f = removeLocal(cname, value, optionsCopy);
  }
  return addTracking(std::move(f), value, [=](OperationResult const& opRes,
                                              VPackSlice data) {
    events::DeleteDocument(vocbase().name(), cname, data,
                            opRes._options, opRes.errorNumber());
  });
}

/// @brief remove one or multiple documents in a collection, coordinator
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
#ifndef USE_ENTERPRISE
Future<OperationResult> transaction::Methods::removeCoordinator(std::string const& cname,
                                                                VPackSlice const value,
                                                                OperationOptions const& options) {
  ClusterInfo& ci = vocbase().server().getFeature<ClusterFeature>().clusterInfo();
  auto colptr = ci.getCollectionNT(vocbase().name(), cname);
  if (colptr == nullptr) {
    return futures::makeFuture(OperationResult(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND));
  }
  return arangodb::removeDocumentOnCoordinator(*this, *colptr, value, options);
}
#endif

/// @brief remove one or multiple documents in a collection, local
/// the single-document variant of this operation will either succeed or,
/// if it fails, clean up after itself
Future<OperationResult> transaction::Methods::removeLocal(std::string const& collectionName,
                                                          VPackSlice const value,
                                                          OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::WRITE);
  auto const& collection = trxCollection(cid)->collection();

  bool const needsLock = !isLocked(collection.get(), AccessMode::Type::WRITE);
  bool const isMMFiles = EngineSelectorFeature::isMMFiles();

  // Assert my assumption that we don't have a lock only with mmfiles single
  // document operations.

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  {
    bool const isMock = EngineSelectorFeature::ENGINE->typeName() == "Mock";
    if (!isMock) {
      // needsLock => isMMFiles
      // needsLock => !value.isArray()
      // needsLock => _localHints.has(Hints::Hint::SINGLE_OPERATION))
      // However, due to nested transactions, there are mmfiles single
      // operations that already have a lock.
      TRI_ASSERT(!needsLock || isMMFiles);
      TRI_ASSERT(!needsLock || !value.isArray());
      TRI_ASSERT(!needsLock || _localHints.has(Hints::Hint::SINGLE_OPERATION));
    }
  }
#endif

  // If we are
  // - not on a single server (i.e. maybe replicating),
  // - using the MMFiles storage engine, and
  // - doing a single document operation,
  // we have to:
  // - Get the list of followers during the time span we actually do hold a
  //   collection level lock. This is to avoid races with the replication where
  //   a follower may otherwise be added between the actual document operation
  //   and the point where we get our copy of the followers, regardless of the
  //   latter happens before or after the document operation.
  bool const needsToGetFollowersUnderLock = needsLock && _state->isDBServer();

  std::shared_ptr<std::vector<ServerID> const> followers;

  std::function<void()> updateFollowers = nullptr;

  if (needsToGetFollowersUnderLock) {
    auto const& followerInfo = *collection->followers();

    updateFollowers = [&followerInfo, &followers]() {
      TRI_ASSERT(followers == nullptr);
      followers = followerInfo.get();
    };
  } else if (_state->isDBServer()) {
    TRI_ASSERT(followers == nullptr);
    followers = collection->followers()->get();
  }

  // we may need to lock individual keys here so we can ensure that even with
  // concurrent operations on the same keys we have the same order of data
  // application on leader and followers
  KeyLockInfo keyLockInfo;

  ReplicationType replicationType = ReplicationType::NONE;
  if (_state->isDBServer()) {
    // Block operation early if we are not supposed to perform it:
    auto const& followerInfo = collection->followers();
    std::string theLeader = followerInfo->getLeader();
    if (theLeader.empty()) {
      if (!options.isSynchronousReplicationFrom.empty()) {
        return OperationResult(TRI_ERROR_CLUSTER_SHARD_LEADER_REFUSES_REPLICATION);
      }
      if (!followerInfo->allowedToWrite()) {
        // We cannot fulfill minimum replication Factor.
        // Reject write.
        LOG_TOPIC("f1f8e", ERR, Logger::REPLICATION)
            << "Less than minReplicationFactor ("
            << basics::StringUtils::itoa(collection->writeConcern())
            << ") followers in sync. Shard  " << collection->name()
            << " is temporarily in read-only mode.";
        return OperationResult(TRI_ERROR_ARANGO_READ_ONLY, options);
      }

      replicationType = ReplicationType::LEADER;
      if (isMMFiles && needsLock) {
        keyLockInfo.shouldLock = true;
      }
      // We cannot be silent if we may have to replicate later.
      // If we need to get the followers under the single document operation's
      // lock, we don't know yet if we will have followers later and thus cannot
      // be silent.
      // Otherwise, if we already know the followers to replicate to, we can
      // just check if they're empty.
      if (needsToGetFollowersUnderLock || !followers->empty()) {
        options.silent = false;
      }
    } else {  // we are a follower following theLeader
      replicationType = ReplicationType::FOLLOWER;
      if (options.isSynchronousReplicationFrom.empty()) {
        return OperationResult(TRI_ERROR_CLUSTER_SHARD_LEADER_RESIGNED);
      }
      if (options.isSynchronousReplicationFrom != theLeader) {
        return OperationResult(TRI_ERROR_CLUSTER_SHARD_FOLLOWER_REFUSES_OPERATION);
      }
    }
  }  // isDBServer - early block

  if (options.returnOld) {
    pinData(cid);  // will throw when it fails
  }

  VPackBuilder resultBuilder;
  ManagedDocumentResult previous;

  auto workForOneDocument = [&](VPackSlice value, bool isBabies) -> Result {
    transaction::BuilderLeaser builder(this);
    arangodb::velocypack::StringRef key;
    if (value.isString()) {
      key = value;
      size_t pos = key.find('/');
      if (pos != std::string::npos) {
        key = key.substr(pos + 1);
        builder->add(VPackValuePair(key.data(), key.length(), VPackValueType::String));
        value = builder->slice();
      }
    } else if (value.isObject()) {
      VPackSlice keySlice = value.get(StaticStrings::KeyString);
      if (!keySlice.isString()) {
        return Result(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
      }
      key = keySlice;
    } else {
      return Result(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
    }

    previous.clear();

    TRI_ASSERT(needsLock == !isLocked(collection.get(), AccessMode::Type::WRITE));

    auto res = collection->remove(*this, value, options, needsLock, previous,
                                  &keyLockInfo, updateFollowers);

    if (res.fail()) {
      if (res.is(TRI_ERROR_ARANGO_CONFLICT) && !isBabies) {
        TRI_ASSERT(previous.revisionId() != 0);
        buildDocumentIdentity(collection.get(), resultBuilder, cid, key,
                              previous.revisionId(), 0,
                              options.returnOld ? &previous : nullptr, nullptr);
      }
      return res;
    }

    if (!options.silent) {
      TRI_ASSERT(!options.returnOld || !previous.empty());
      TRI_ASSERT(previous.revisionId() != 0);
      buildDocumentIdentity(collection.get(), resultBuilder, cid, key,
                            previous.revisionId(), 0,
                            options.returnOld ? &previous : nullptr, nullptr);
    }

    return res;
  };

  Result res;
  std::unordered_map<int, size_t> errorCounter;
  if (value.isArray()) {
    VPackArrayBuilder guard(&resultBuilder);
    for (VPackSlice s : VPackArrayIterator(value)) {
      res = workForOneDocument(s, true);
      if (res.fail()) {
        createBabiesError(resultBuilder, errorCounter, res);
      }
    }
    res.reset(); // With babies reporting is handled in the result body
  } else {
    res = workForOneDocument(value, false);
  }

  auto resDocs = resultBuilder.steal();
  if (res.ok() && replicationType == ReplicationType::LEADER) {
    TRI_ASSERT(collection != nullptr);
    TRI_ASSERT(followers != nullptr);
    // Now replicate the same operation on all followers:

    // In the multi babies case res is always TRI_ERROR_NO_ERROR if we
    // get here, in the single document case, we do not try to replicate
    // in case of an error.

    // Now replicate the good operations on all followers:
    return replicateOperations(collection.get(), followers, options, value,
                               TRI_VOC_DOCUMENT_OPERATION_REMOVE, resDocs)
    .thenValue([options, errs = std::move(errorCounter), resDocs](Result res) {
      if (!res.ok()) {
        return OperationResult{std::move(res), options};
      }
      if (options.silent && errs.empty()) {
        // We needed the results, but do not want to report:
        resDocs->clear();
      }
      return OperationResult(std::move(res), std::move(resDocs),
                             std::move(options), std::move(errs));
    });
  }

  if (options.silent && errorCounter.empty()) {
    // We needed the results, but do not want to report:
    resDocs->clear();
  }

  return OperationResult(std::move(res), std::move(resDocs),
                         std::move(options), std::move(errorCounter));
}

/// @brief fetches all documents in a collection
OperationResult transaction::Methods::all(std::string const& collectionName,
                                          uint64_t skip, uint64_t limit,
                                          OperationOptions const& options) {
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  OperationOptions optionsCopy = options;

  if (_state->isCoordinator()) {
    return allCoordinator(collectionName, skip, limit, optionsCopy);
  }

  return allLocal(collectionName, skip, limit, optionsCopy);
}

/// @brief fetches all documents in a collection, coordinator
OperationResult transaction::Methods::allCoordinator(std::string const& collectionName,
                                                     uint64_t skip, uint64_t limit,
                                                     OperationOptions& options) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

/// @brief fetches all documents in a collection, local
OperationResult transaction::Methods::allLocal(std::string const& collectionName,
                                               uint64_t skip, uint64_t limit,
                                               OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::READ);

  pinData(cid);  // will throw when it fails

  VPackBuilder resultBuilder;
  resultBuilder.openArray();

  Result lockResult = lockRecursive(cid, AccessMode::Type::READ);

  if (!lockResult.ok() && !lockResult.is(TRI_ERROR_LOCKED)) {
    return OperationResult(lockResult);
  }

  OperationCursor cursor(indexScan(collectionName, transaction::Methods::CursorType::ALL));

  auto cb = [&resultBuilder](LocalDocumentId const& token, VPackSlice slice) {
    resultBuilder.add(slice);
    return true;
  };
  cursor.allDocuments(cb, 1000);

  if (lockResult.is(TRI_ERROR_LOCKED)) {
    Result res = unlockRecursive(cid, AccessMode::Type::READ);

    if (res.ok()) {
      return OperationResult(res);
    }
  }

  resultBuilder.close();

  return OperationResult(Result(), resultBuilder.steal());
}

/// @brief remove all documents in a collection
Future<OperationResult> transaction::Methods::truncateAsync(std::string const& collectionName,
                                                            OperationOptions const& options) {
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  OperationOptions optionsCopy = options;
  auto cb = [this, collectionName](OperationResult res) {
    events::TruncateCollection(vocbase().name(), collectionName, res.errorNumber());
    return res;
  };

  if (_state->isCoordinator()) {
    return truncateCoordinator(collectionName, optionsCopy).thenValue(cb);
  }
  return truncateLocal(collectionName, optionsCopy).thenValue(cb);
}

/// @brief remove all documents in a collection, coordinator
#ifndef USE_ENTERPRISE
Future<OperationResult> transaction::Methods::truncateCoordinator(std::string const& collectionName,
                                                                  OperationOptions& options) {
  return arangodb::truncateCollectionOnCoordinator(*this, collectionName);
}
#endif

/// @brief remove all documents in a collection, local
Future<OperationResult> transaction::Methods::truncateLocal(std::string const& collectionName,
                                                            OperationOptions& options) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::WRITE);

  auto const& collection = trxCollection(cid)->collection();

  std::shared_ptr<std::vector<ServerID> const> followers;

  ReplicationType replicationType = ReplicationType::NONE;
  if (_state->isDBServer()) {
    // Block operation early if we are not supposed to perform it:
    auto const& followerInfo = collection->followers();
    std::string theLeader = followerInfo->getLeader();
    if (theLeader.empty()) {
      if (!options.isSynchronousReplicationFrom.empty()) {
        return futures::makeFuture(
            OperationResult(TRI_ERROR_CLUSTER_SHARD_LEADER_REFUSES_REPLICATION));
      }
      if (!followerInfo->allowedToWrite()) {
        // We cannot fulfill minimum replication Factor.
        // Reject write.
        LOG_TOPIC("7c1d4", ERR, Logger::REPLICATION)
            << "Less than minReplicationFactor ("
            << basics::StringUtils::itoa(collection->writeConcern())
            << ") followers in sync. Shard  " << collection->name()
            << " is temporarily in read-only mode.";
        return futures::makeFuture(OperationResult(TRI_ERROR_ARANGO_READ_ONLY, options));
      }

      // fetch followers
      followers = followerInfo->get();
      if (followers->size() > 0) {
        replicationType = ReplicationType::LEADER;
        options.silent = false;
      }
    } else {  // we are a follower following theLeader
      replicationType = ReplicationType::FOLLOWER;
      if (options.isSynchronousReplicationFrom.empty()) {
        return futures::makeFuture(OperationResult(TRI_ERROR_CLUSTER_SHARD_LEADER_RESIGNED));
      }
      if (options.isSynchronousReplicationFrom != theLeader) {
        return futures::makeFuture(
            OperationResult(TRI_ERROR_CLUSTER_SHARD_FOLLOWER_REFUSES_OPERATION));
      }
    }
  }  // isDBServer - early block

  pinData(cid);  // will throw when it fails

  Result lockResult = lockRecursive(cid, AccessMode::Type::WRITE);

  if (!lockResult.ok() && !lockResult.is(TRI_ERROR_LOCKED)) {
    return futures::makeFuture(OperationResult(lockResult));
  }

  TRI_ASSERT(isLocked(collection.get(), AccessMode::Type::WRITE));

  auto res = collection->truncate(*this, options);

  if (res.fail()) {
    if (lockResult.is(TRI_ERROR_LOCKED)) {
      unlockRecursive(cid, AccessMode::Type::WRITE);
    }

    return futures::makeFuture(OperationResult(res));
  }

  // Now see whether or not we have to do synchronous replication:
  if (replicationType == ReplicationType::LEADER) {
    TRI_ASSERT(followers != nullptr);

    TRI_ASSERT(!_state->hasHint(Hints::Hint::FROM_TOPLEVEL_AQL));

    // Now replicate the good operations on all followers:
    NetworkFeature const& nf = vocbase().server().getFeature<NetworkFeature>();
    network::ConnectionPool* pool = nf.pool();
    if (pool != nullptr) {
      // nullptr only happens on controlled shutdown
      std::string path =
          "/_api/collection/" + arangodb::basics::StringUtils::urlEncode(collectionName) +
          "/truncate";
      VPackBuffer<uint8_t> body;
      VPackSlice s = VPackSlice::emptyObjectSlice();
      body.append(s.start(), s.byteSize());

      // Now prepare the requests:
      std::vector<network::FutureRes> futures;
      futures.reserve(followers->size());
      
      network::RequestOptions reqOpts;
      reqOpts.database = vocbase().name();
      reqOpts.timeout = network::Timeout(600);
      reqOpts.param(StaticStrings::IsSynchronousReplicationString, ServerState::instance()->getId());

      for (auto const& f : *followers) {
        network::Headers headers;
        ClusterTrxMethods::addTransactionHeader(*this, f, headers);
        auto future = network::sendRequest(pool, "server:" + f, fuerte::RestVerb::Put,
                                           path, body, reqOpts, std::move(headers));
        futures.emplace_back(std::move(future));
      }

      auto responses = futures::collectAll(futures).get();
      // If any would-be-follower refused to follow there must be a
      // new leader in the meantime, in this case we must not allow
      // this operation to succeed, we simply return with a refusal
      // error (note that we use the follower version, since we have
      // lost leadership):
      if (findRefusal(responses)) {
        return futures::makeFuture(OperationResult(TRI_ERROR_CLUSTER_SHARD_LEADER_RESIGNED));
      }
      // we drop all followers that were not successful:
      for (size_t i = 0; i < followers->size(); ++i) {
        bool replicationWorked =
            responses[i].hasValue() && responses[i].get().ok() &&
            (responses[i].get().response->statusCode() == fuerte::StatusAccepted ||
             responses[i].get().response->statusCode() == fuerte::StatusOK);
        if (!replicationWorked) {
          auto const& followerInfo = collection->followers();
          Result res = followerInfo->remove((*followers)[i]);
          if (res.ok()) {
            _state->removeKnownServer((*followers)[i]);
            LOG_TOPIC("0e2e0", WARN, Logger::REPLICATION)
                << "truncateLocal: dropping follower " << (*followers)[i]
                << " for shard " << collectionName;
          } else {
            LOG_TOPIC("359bc", WARN, Logger::REPLICATION)
                << "truncateLocal: could not drop follower " << (*followers)[i]
                << " for shard " << collectionName << ": " << res.errorMessage();
            THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_COULD_NOT_DROP_FOLLOWER);
          }
        }
      }
    }
  }

  if (lockResult.is(TRI_ERROR_LOCKED)) {
    res = unlockRecursive(cid, AccessMode::Type::WRITE);
  }

  return futures::makeFuture(OperationResult(res));
}

/// @brief count the number of documents in a collection
futures::Future<OperationResult> transaction::Methods::countAsync(std::string const& collectionName,
                                                                  transaction::CountType type) {
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  if (_state->isCoordinator()) {
    return countCoordinator(collectionName, type);
  }

  if (type == CountType::Detailed) {
    // we are a single-server... we cannot provide detailed per-shard counts,
    // so just downgrade the request to a normal request
    type = CountType::Normal;
  }

  return futures::makeFuture(countLocal(collectionName, type));
}

#ifndef USE_ENTERPRISE
/// @brief count the number of documents in a collection
futures::Future<OperationResult> transaction::Methods::countCoordinator(
    std::string const& collectionName, transaction::CountType type) {
  auto& feature = vocbase().server().getFeature<ClusterFeature>();
  ClusterInfo& ci = feature.clusterInfo();

  // First determine the collection ID from the name:
  auto collinfo = ci.getCollectionNT(vocbase().name(), collectionName);
  if (collinfo == nullptr) {
    return futures::makeFuture(OperationResult(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND));
  }

  return countCoordinatorHelper(collinfo, collectionName, type);
}

#endif

futures::Future<OperationResult> transaction::Methods::countCoordinatorHelper(
    std::shared_ptr<LogicalCollection> const& collinfo,
    std::string const& collectionName, transaction::CountType type) {
  TRI_ASSERT(collinfo != nullptr);
  auto& cache = collinfo->countCache();

  int64_t documents = CountCache::NotPopulated;
  if (type == transaction::CountType::ForceCache) {
    // always return from the cache, regardless what's in it
    documents = cache.get();
  } else if (type == transaction::CountType::TryCache) {
    documents = cache.get(CountCache::Ttl);
  }

  if (documents == CountCache::NotPopulated) {
    // no cache hit, or detailed results requested
    return arangodb::countOnCoordinator(*this, collectionName)
        .thenValue([&cache, type](OperationResult&& res) -> OperationResult {
          if (res.fail()) {
            return std::move(res);
          }

          // reassemble counts from vpack
          std::vector<std::pair<std::string, uint64_t>> counts;
          TRI_ASSERT(res.slice().isArray());
          for (VPackSlice count : VPackArrayIterator(res.slice())) {
            TRI_ASSERT(count.isArray());
            TRI_ASSERT(count[0].isString());
            TRI_ASSERT(count[1].isNumber());
            std::string key = count[0].copyString();
            uint64_t value = count[1].getNumericValue<uint64_t>();
            counts.emplace_back(std::move(key), value);
          }

          int64_t total = 0;
          OperationResult opRes = buildCountResult(counts, type, total);
          cache.store(total);
          return opRes;
        });
  }

  // cache hit!
  TRI_ASSERT(documents >= 0);
  TRI_ASSERT(type != transaction::CountType::Detailed);

  // return number from cache
  VPackBuilder resultBuilder;
  resultBuilder.add(VPackValue(documents));
  return OperationResult(Result(), resultBuilder.steal());
}

/// @brief count the number of documents in a collection
OperationResult transaction::Methods::countLocal(std::string const& collectionName,
                                                 transaction::CountType type) {
  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::READ);
  auto const& collection = trxCollection(cid)->collection();

  Result lockResult = lockRecursive(cid, AccessMode::Type::READ);

  if (!lockResult.ok() && !lockResult.is(TRI_ERROR_LOCKED)) {
    return OperationResult(lockResult);
  }

  TRI_ASSERT(isLocked(collection.get(), AccessMode::Type::READ));

  uint64_t num = collection->numberDocuments(this, type);

  if (lockResult.is(TRI_ERROR_LOCKED)) {
    Result res = unlockRecursive(cid, AccessMode::Type::READ);

    if (res.fail()) {
      return OperationResult(res);
    }
  }

  VPackBuilder resultBuilder;
  resultBuilder.add(VPackValue(num));

  return OperationResult(Result(), resultBuilder.steal());
}

/// @brief Gets the best fitting index for an AQL condition.
/// note: the caller must have read-locked the underlying collection when
/// calling this method
std::pair<bool, bool> transaction::Methods::getBestIndexHandlesForFilterCondition(
    std::string const& collectionName, arangodb::aql::Ast* ast,
    arangodb::aql::AstNode* root, arangodb::aql::Variable const* reference,
    arangodb::aql::SortCondition const* sortCondition, size_t itemsInCollection,
    aql::IndexHint const& hint, std::vector<IndexHandle>& usedIndexes, bool& isSorted) {
  // We can only start after DNF transformation
  TRI_ASSERT(root->type == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_NARY_OR);
  auto indexes = indexesForCollection(collectionName);

  // must edit root in place; TODO change so we can replace with copy
  TEMPORARILY_UNLOCK_NODE(root);

  bool canUseForFilter = (root->numMembers() > 0);
  bool canUseForSort = false;
  bool isSparse = false;

  for (size_t i = 0; i < root->numMembers(); ++i) {
    auto node = root->getMemberUnchecked(i);
    arangodb::aql::AstNode* specializedCondition = nullptr;
    auto canUseIndex = findIndexHandleForAndNode(indexes, node, reference, *sortCondition,
                                                 itemsInCollection, hint, usedIndexes,
                                                 specializedCondition, isSparse);

    if (canUseIndex.second && !canUseIndex.first) {
      // index can be used for sorting only
      // we need to abort further searching and only return one index
      TRI_ASSERT(!usedIndexes.empty());
      if (usedIndexes.size() > 1) {
        auto sortIndex = usedIndexes.back();

        usedIndexes.clear();
        usedIndexes.emplace_back(sortIndex);
      }

      TRI_ASSERT(usedIndexes.size() == 1);

      if (isSparse) {
        // cannot use a sparse index for sorting alone
        usedIndexes.clear();
      }
      return std::make_pair(false, !usedIndexes.empty());
    }

    canUseForFilter &= canUseIndex.first;
    canUseForSort |= canUseIndex.second;

    root->changeMember(i, specializedCondition);
  }

  if (canUseForFilter) {
    isSorted = sortOrs(ast, root, reference, usedIndexes);
  }

  // should always be true here. maybe not in the future in case a collection
  // has absolutely no indexes
  return std::make_pair(canUseForFilter, canUseForSort);
}

/// @brief Gets the best fitting index for one specific condition.
///        Difference to IndexHandles: Condition is only one NARY_AND
///        and the Condition stays unmodified. Also does not care for sorting
///        Returns false if no index could be found.

bool transaction::Methods::getBestIndexHandleForFilterCondition(
    std::string const& collectionName, arangodb::aql::AstNode*& node,
    arangodb::aql::Variable const* reference, size_t itemsInCollection,
    aql::IndexHint const& hint, IndexHandle& usedIndex) {
  // We can only start after DNF transformation and only a single AND
  TRI_ASSERT(node->type == arangodb::aql::AstNodeType::NODE_TYPE_OPERATOR_NARY_AND);
  if (node->numMembers() == 0) {
    // Well no index can serve no condition.
    return false;
  }

  arangodb::aql::SortCondition sortCondition;    // always empty here
  arangodb::aql::AstNode* specializedCondition;  // unused
  bool isSparse;                                 // unused
  std::vector<IndexHandle> usedIndexes;
  if (findIndexHandleForAndNode(indexesForCollection(collectionName), node,
                                reference, sortCondition, itemsInCollection,
                                hint, usedIndexes, specializedCondition, isSparse)
          .first) {
    TRI_ASSERT(!usedIndexes.empty());
    usedIndex = usedIndexes[0];
    return true;
  }
  return false;
}

/// @brief Gets the best fitting index for an AQL sort condition
/// note: the caller must have read-locked the underlying collection when
/// calling this method
bool transaction::Methods::getIndexForSortCondition(
    std::string const& collectionName, arangodb::aql::SortCondition const* sortCondition,
    arangodb::aql::Variable const* reference, size_t itemsInIndex,
    aql::IndexHint const& hint, std::vector<IndexHandle>& usedIndexes,
    size_t& coveredAttributes) {
  // We do not have a condition. But we have a sort!
  if (!sortCondition->isEmpty() && sortCondition->isOnlyAttributeAccess() &&
      sortCondition->isUnidirectional()) {
    double bestCost = 0.0;
    std::shared_ptr<Index> bestIndex;

    auto considerIndex = [reference, sortCondition, itemsInIndex, &bestCost, &bestIndex,
                          &coveredAttributes](std::shared_ptr<Index> const& idx) -> void {
      TRI_ASSERT(!idx->inProgress());

      Index::SortCosts costs =
          idx->supportsSortCondition(sortCondition, reference, itemsInIndex);
      if (costs.supportsCondition &&
          (bestIndex == nullptr || costs.estimatedCosts < bestCost)) {
        bestCost = costs.estimatedCosts;
        bestIndex = idx;
        coveredAttributes = costs.coveredAttributes;
      }
    };

    auto indexes = indexesForCollection(collectionName);

    if (hint.type() == aql::IndexHint::HintType::Simple) {
      std::vector<std::string> const& hintedIndices = hint.hint();
      for (std::string const& hinted : hintedIndices) {
        std::shared_ptr<Index> matched;
        for (std::shared_ptr<Index> const& idx : indexes) {
          if (idx->name() == hinted) {
            matched = idx;
            break;
          }
        }

        if (matched != nullptr) {
          considerIndex(matched);
          if (bestIndex != nullptr) {
            break;
          }
        }
      }

      if (hint.isForced() && bestIndex == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_QUERY_FORCED_INDEX_HINT_UNUSABLE,
            "could not use index hint to serve query; " + hint.toString());
      }
    }

    if (bestIndex == nullptr) {
      for (auto const& idx : indexes) {
        considerIndex(idx);
      }
    }

    if (bestIndex != nullptr) {
      usedIndexes.emplace_back(bestIndex);
    }

    return bestIndex != nullptr;
  }

  // No Index and no sort condition that
  // can be supported by an index.
  // Nothing to do here.
  return false;
}

/// @brief factory for IndexIterator objects from AQL
/// note: the caller must have read-locked the underlying collection when
/// calling this method
std::unique_ptr<IndexIterator> transaction::Methods::indexScanForCondition(
    IndexHandle const& idx, arangodb::aql::AstNode const* condition,
    arangodb::aql::Variable const* var, IndexIteratorOptions const& opts) {
  if (_state->isCoordinator()) {
    // The index scan is only available on DBServers and Single Server.
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_ONLY_ON_DBSERVER);
  }

  if (nullptr == idx) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The index id cannot be empty.");
  }

  // Now create the Iterator
  TRI_ASSERT(!idx->inProgress());
  return idx->iteratorForCondition(this, condition, var, opts);
}

/// @brief factory for IndexIterator objects
/// note: the caller must have read-locked the underlying collection when
/// calling this method
std::unique_ptr<IndexIterator> transaction::Methods::indexScan(std::string const& collectionName,
                                                               CursorType cursorType) {
  // For now we assume indexId is the iid part of the index.

  if (_state->isCoordinator()) {
    // The index scan is only available on DBServers and Single Server.
    THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_ONLY_ON_DBSERVER);
  }

  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::READ);
  TransactionCollection* trxColl = trxCollection(cid);
  if (trxColl == nullptr) {
    throwCollectionNotFound(collectionName.c_str());
  }
  std::shared_ptr<LogicalCollection> const& logical = trxColl->collection();
  TRI_ASSERT(logical != nullptr);

  // will throw when it fails
  _transactionContextPtr->pinData(logical.get());

  std::unique_ptr<IndexIterator> iterator;
  switch (cursorType) {
    case CursorType::ANY: {
      iterator = logical->getAnyIterator(this);
      break;
    }
    case CursorType::ALL: {
      iterator = logical->getAllIterator(this);
      break;
    }
  }

  // the above methods must always return a valid iterator or throw!
  TRI_ASSERT(iterator != nullptr);
  return iterator;
}

/// @brief return the collection
arangodb::LogicalCollection* transaction::Methods::documentCollection(TRI_voc_cid_t cid) const {
  TRI_ASSERT(_state != nullptr);
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  auto trxColl = trxCollection(cid, AccessMode::Type::READ);
  if (trxColl == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "could not find collection");
  }

  TRI_ASSERT(trxColl != nullptr);
  TRI_ASSERT(trxColl->collection() != nullptr);
  return trxColl->collection().get();
}

/// @brief return the collection
arangodb::LogicalCollection* transaction::Methods::documentCollection(std::string const& name) const {
  TRI_ASSERT(_state != nullptr);
  TRI_ASSERT(_state->status() == transaction::Status::RUNNING);

  auto trxColl = trxCollection(name, AccessMode::Type::READ);
  if (trxColl == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "could not find collection");
  }

  TRI_ASSERT(trxColl != nullptr);
  TRI_ASSERT(trxColl->collection() != nullptr);
  return trxColl->collection().get();
}

/// @brief add a collection by id, with the name supplied
Result transaction::Methods::addCollection(TRI_voc_cid_t cid, std::string const& cname,
                                           AccessMode::Type type) {
  if (_state == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "cannot add collection without state");
  }

  Status const status = _state->status();

  if (status == transaction::Status::COMMITTED || status == transaction::Status::ABORTED) {
    // transaction already finished?
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "cannot add collection to committed or aborted transaction");
  }

  if (_state->isTopLevelTransaction() && status != transaction::Status::CREATED) {
    // transaction already started?
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_TRANSACTION_INTERNAL,
        "cannot add collection to a previously started top-level transaction");
  }

  if (cid == 0) {
    // invalid cid
    throwCollectionNotFound(cname.c_str());
  }

  auto addCollectionCallback = [this, &cname, type](TRI_voc_cid_t cid) -> void {
    auto res = _state->addCollection(cid, cname, type, _state->nestingLevel(), false);

    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
  };

  Result res;
  bool visited = false;
  std::function<bool(LogicalCollection&)> visitor(
      [this, &addCollectionCallback, &res, cid, &visited](LogicalCollection& col) -> bool {
        addCollectionCallback(col.id());  // will throw on error
        res = applyDataSourceRegistrationCallbacks(col, *this);
        visited |= cid == col.id();

        return res.ok();  // add the remaining collections (or break on error)
      });

  if (!resolver()->visitCollections(visitor, cid) || res.fail()) {
    // trigger exception as per the original behavior (tests depend on this)
    if (res.ok() && !visited) {
      addCollectionCallback(cid);  // will throw on error
    }

    return res.ok() ? Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) : res;  // return first error
  }

  // skip provided 'cid' if it was already done by the visitor
  if (visited) {
    return res;
  }

  auto dataSource = resolver()->getDataSource(cid);

  return dataSource ? applyDataSourceRegistrationCallbacks(*dataSource, *this)
                    : Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
}

/// @brief add a collection by name
Result transaction::Methods::addCollection(std::string const& name, AccessMode::Type type) {
  return addCollection(resolver()->getCollectionId(name), name, type);
}

/// @brief test if a collection is already locked
bool transaction::Methods::isLocked(LogicalCollection* document, AccessMode::Type type) const {
  if (_state == nullptr || _state->status() != transaction::Status::RUNNING) {
    return false;
  }
  if (_state->hasHint(Hints::Hint::LOCK_NEVER)) {
    // In the lock never case we have made sure that
    // some other process holds this lock.
    // So we can lie here and report that it actually
    // is locked!
    return true;
  }

  TransactionCollection* trxColl = trxCollection(document->id(), type);
  TRI_ASSERT(trxColl != nullptr);
  return trxColl->isLocked(type, _state->nestingLevel());
}

/// @brief read- or write-lock a collection
Result transaction::Methods::lockRecursive(TRI_voc_cid_t cid, AccessMode::Type type) {
  if (_state == nullptr || _state->status() != transaction::Status::RUNNING) {
    return Result(TRI_ERROR_TRANSACTION_INTERNAL,
                  "transaction not running on lock");
  }
  TransactionCollection* trxColl = trxCollection(cid, type);
  TRI_ASSERT(trxColl != nullptr);
  return Result(trxColl->lockRecursive(type, _state->nestingLevel()));
}

/// @brief read- or write-unlock a collection
Result transaction::Methods::unlockRecursive(TRI_voc_cid_t cid, AccessMode::Type type) {
  if (_state == nullptr || _state->status() != transaction::Status::RUNNING) {
    return Result(TRI_ERROR_TRANSACTION_INTERNAL,
                  "transaction not running on unlock");
  }
  TransactionCollection* trxColl = trxCollection(cid, type);
  TRI_ASSERT(trxColl != nullptr);
  return Result(trxColl->unlockRecursive(type, _state->nestingLevel()));
}

/// @brief get list of indexes for a collection
std::vector<std::shared_ptr<Index>> transaction::Methods::indexesForCollection(
    std::string const& collectionName) {
  if (_state->isCoordinator()) {
    return indexesForCollectionCoordinator(collectionName);
  }
  // For a DBserver we use the local case.

  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::READ);
  std::shared_ptr<LogicalCollection> const& document = trxCollection(cid)->collection();
  std::vector<std::shared_ptr<Index>> indexes = document->getIndexes();
    
  indexes.erase(std::remove_if(indexes.begin(), indexes.end(),
                               [](std::shared_ptr<Index> const& x) {
                                 return x->isHidden();
                               }),
                indexes.end());
  return indexes;
}

/// @brief Lock all collections. Only works for selected sub-classes
int transaction::Methods::lockCollections() {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

/// @brief Get all indexes for a collection name, coordinator case
std::shared_ptr<Index> transaction::Methods::indexForCollectionCoordinator(
    std::string const& name, std::string const& id) const {
  auto& ci = vocbase().server().getFeature<ClusterFeature>().clusterInfo();
  auto collection = ci.getCollection(vocbase().name(), name);
  TRI_idx_iid_t iid = basics::StringUtils::uint64(id);
  return collection->lookupIndex(iid);
}

/// @brief Get all indexes for a collection name, coordinator case
std::vector<std::shared_ptr<Index>> transaction::Methods::indexesForCollectionCoordinator(
    std::string const& name) const {
  auto& ci = vocbase().server().getFeature<ClusterFeature>().clusterInfo();
  auto collection = ci.getCollection(vocbase().name(), name);

  // update selectivity estimates if they were expired
  if (_state->hasHint(Hints::Hint::GLOBAL_MANAGED)) {  // hack to fix mmfiles
    collection->clusterIndexEstimates(true, _state->id() + 1);
  } else {
    collection->clusterIndexEstimates(true);
  }

  std::vector<std::shared_ptr<Index>> indexes = collection->getIndexes();
    
  indexes.erase(std::remove_if(indexes.begin(), indexes.end(),
                               [](std::shared_ptr<Index> const& x) {
                                 return x->isHidden();
                               }),
                indexes.end());
  return indexes;
}

/// @brief get the index by it's identifier. Will either throw or
///        return a valid index. nullptr is impossible.
transaction::Methods::IndexHandle transaction::Methods::getIndexByIdentifier(
    std::string const& collectionName, std::string const& idxId) {
  
  if (idxId.empty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The index id cannot be empty.");
  }

  if (!arangodb::Index::validateId(idxId.c_str())) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_INDEX_HANDLE_BAD);
  }
  
  if (_state->isCoordinator()) {
    std::shared_ptr<Index> idx = indexForCollectionCoordinator(collectionName, idxId);
    if (idx == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_INDEX_NOT_FOUND,
                                     "Could not find index '" + idxId +
                                         "' in collection '" + collectionName +
                                         "'.");
    }

    // We have successfully found an index with the requested id.
    return IndexHandle(idx);
  }

  TRI_voc_cid_t cid = addCollectionAtRuntime(collectionName, AccessMode::Type::READ);
  std::shared_ptr<LogicalCollection> const& document = trxCollection(cid)->collection();


  TRI_idx_iid_t iid = arangodb::basics::StringUtils::uint64(idxId);
  std::shared_ptr<arangodb::Index> idx = document->lookupIndex(iid);

  if (idx == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_INDEX_NOT_FOUND,
                                   "Could not find index '" + idxId +
                                       "' in collection '" + collectionName +
                                       "'.");
  }

  // We have successfully found an index with the requested id.
  return IndexHandle(idx);
}

Result transaction::Methods::resolveId(char const* handle, size_t length,
                                       std::shared_ptr<LogicalCollection>& collection,
                                       char const*& key, size_t& outLength) {
  char const* p =
      static_cast<char const*>(memchr(handle, TRI_DOCUMENT_HANDLE_SEPARATOR_CHR, length));

  if (p == nullptr || *p == '\0') {
    return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
  }

  std::string const name(handle, p - handle);
  collection = resolver()->getCollectionStructCluster(name);

  if (collection == nullptr) {
    return TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND;
  }

  key = p + 1;
  outLength = length - (key - handle);

  return TRI_ERROR_NO_ERROR;
}

// Unified replication of operations. May be inserts (with or without
// overwrite), removes, or modifies (updates/replaces).
Future<Result> Methods::replicateOperations(
    LogicalCollection* collection,
    std::shared_ptr<const std::vector<ServerID>> const& followerList,
    OperationOptions const& options, VPackSlice const value,
    TRI_voc_document_operation_e const operation,
    std::shared_ptr<VPackBuffer<uint8_t>> const& ops) {
  TRI_ASSERT(followerList != nullptr);

  if (followerList->empty()) {
    return Result();
  }

  // path and requestType are different for insert/remove/modify.
  
  network::RequestOptions reqOpts;
  reqOpts.database = vocbase().name();
  reqOpts.param(StaticStrings::IsRestoreString, "true");
  reqOpts.param(StaticStrings::IsSynchronousReplicationString, ServerState::instance()->getId());

  std::string url = "/_api/document/";
  url.append(arangodb::basics::StringUtils::urlEncode(collection->name()));
  if (operation != TRI_VOC_DOCUMENT_OPERATION_INSERT && !value.isArray()) {
    TRI_ASSERT(value.isObject());
    TRI_ASSERT(value.hasKey(StaticStrings::KeyString));
    url.push_back('/');
    VPackValueLength len;
    const char* ptr = value.get(StaticStrings::KeyString).getString(len);
    url.append(ptr, len);
  }

  arangodb::fuerte::RestVerb requestType = arangodb::fuerte::RestVerb::Illegal;
  switch (operation) {
    case TRI_VOC_DOCUMENT_OPERATION_INSERT:
      requestType = arangodb::fuerte::RestVerb::Post;
      reqOpts.param(StaticStrings::OverWrite, (options.overwrite ? "true" : "false"));
      break;
    case TRI_VOC_DOCUMENT_OPERATION_UPDATE:
      requestType = arangodb::fuerte::RestVerb::Patch;
      break;
    case TRI_VOC_DOCUMENT_OPERATION_REPLACE:
      requestType = arangodb::fuerte::RestVerb::Put;
      break;
    case TRI_VOC_DOCUMENT_OPERATION_REMOVE:
      requestType = arangodb::fuerte::RestVerb::Delete;
      break;
    case TRI_VOC_DOCUMENT_OPERATION_UNKNOWN:
    default:
      TRI_ASSERT(false);
  }

  transaction::BuilderLeaser payload(this);
  auto doOneDoc = [&](VPackSlice const& doc, VPackSlice result) {
    VPackObjectBuilder guard(payload.get());
    VPackSlice s = result.get(StaticStrings::KeyString);
    payload->add(StaticStrings::KeyString, s);
    s = result.get(StaticStrings::RevString);
    payload->add(StaticStrings::RevString, s);
    if (operation != TRI_VOC_DOCUMENT_OPERATION_REMOVE) {
      TRI_SanitizeObject(doc, *payload.get());
    }
  };

  VPackSlice ourResult(ops->data());
  size_t count = 0;
  if (value.isArray()) {
    VPackArrayBuilder guard(payload.get());
    VPackArrayIterator itValue(value);
    VPackArrayIterator itResult(ourResult);
    while (itValue.valid() && itResult.valid()) {
      TRI_ASSERT((*itResult).isObject());
      if (!(*itResult).hasKey(StaticStrings::Error)) {
        doOneDoc(itValue.value(), itResult.value());
        count++;
      }
      itValue.next();
      itResult.next();
    }
  } else {
    doOneDoc(value, ourResult);
    count++;
  }

  if (count == 0) {
    // nothing to do
    return Result();
  }
  
  reqOpts.timeout = network::Timeout(chooseTimeout(count, payload->size()));
  
  // Now prepare the requests:
  std::vector<Future<network::Response>> futures;
  futures.reserve(followerList->size());

  auto* pool = vocbase().server().getFeature<NetworkFeature>().pool();
  for (auto const& f : *followerList) {
    network::Headers headers;
    ClusterTrxMethods::addTransactionHeader(*this, f, headers);
    futures.emplace_back(network::sendRequestRetry(pool, "server:" + f, requestType,
                                                   url, *(payload->buffer()), reqOpts,
                                                   std::move(headers)));
  }

  // If any would-be-follower refused to follow there are two possiblities:
  // (1) there is a new leader in the meantime, or
  // (2) the follower was restarted and forgot that it is a follower.
  // Unfortunately, we cannot know which is the case.
  // In case (1) case we must not allow
  // this operation to succeed, since the new leader is now responsible.
  // In case (2) we at least have to drop the follower such that it
  // resyncs and we can be sure that it is in sync again.
  // Therefore, we drop the follower here (just in case), and refuse to
  // return with a refusal error (note that we use the follower version,
  // since we have lost leadership):
  auto cb = [=](std::vector<futures::Try<network::Response>>&& responses) -> Result {

    bool didRefuse = false;
    // We drop all followers that were not successful:
    for (size_t i = 0; i < followerList->size(); ++i) {
      auto const& tryRes = responses[i];
      network::Response const& resp = tryRes.get();

      bool replicationWorked = false;
      if (resp.error == fuerte::Error::NoError) {
        replicationWorked = resp.response->statusCode() == fuerte::StatusAccepted ||
                            resp.response->statusCode() == fuerte::StatusCreated ||
                            resp.response->statusCode() == fuerte::StatusOK;
        if (replicationWorked) {
          bool found;
          std::string val = resp.response->header.metaByKey(StaticStrings::ErrorCodes, found);
          replicationWorked = !found;
        }
        didRefuse = didRefuse || resp.response->statusCode() == fuerte::StatusNotAcceptable;
      }

      if (!replicationWorked) {
        ServerID const& deadFollower = (*followerList)[i];
        Result res = collection->followers()->remove(deadFollower);
        if (res.ok()) {
          // TODO: what happens if a server is re-added during a transaction ?
          _state->removeKnownServer(deadFollower);
          LOG_TOPIC("12d8c", WARN, Logger::REPLICATION)
              << "synchronous replication: dropping follower "
              << deadFollower << " for shard " << collection->name();
        } else {
          LOG_TOPIC("db473", ERR, Logger::REPLICATION)
              << "synchronous replication: could not drop follower "
              << deadFollower << " for shard " << collection->name() << ": "
              << res.errorMessage();
          THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_COULD_NOT_DROP_FOLLOWER);
        }
      }
    }

    if (didRefuse) {  // case (1), caller may abort this transaction
      return Result(TRI_ERROR_CLUSTER_SHARD_LEADER_RESIGNED);
    }
    return Result();
  };
  return futures::collectAll(std::move(futures)).thenValue(std::move(cb));
}

#ifndef USE_ENTERPRISE
/*static*/ int Methods::validateSmartJoinAttribute(LogicalCollection const&,
                                                   arangodb::velocypack::Slice) {
  return TRI_ERROR_NO_ERROR;
}
#endif
