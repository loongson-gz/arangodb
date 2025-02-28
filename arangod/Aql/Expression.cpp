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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "Expression.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/AqlValue.h"
#include "Aql/Ast.h"
#include "Aql/AttributeAccessor.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/ExpressionContext.h"
#include "Aql/Function.h"
#include "Aql/Functions.h"
#include "Aql/Quantifier.h"
#include "Aql/Query.h"
#include "Aql/Range.h"
#include "Aql/V8Executor.h"
#include "Aql/Variable.h"
#include "Aql/AqlValueMaterializer.h"
#include "Basics/Exceptions.h"
#include "Basics/NumberUtils.h"
#include "Basics/StringBuffer.h"
#include "Basics/VPackStringBufferAdapter.h"
#include "Basics/VelocyPackHelper.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "V8/v8-globals.h"
#include "V8/v8-vpack.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/StringRef.h>
#include <velocypack/velocypack-aliases.h>

#include <v8.h>

using namespace arangodb;
using namespace arangodb::aql;
using VelocyPackHelper = arangodb::basics::VelocyPackHelper;

/// @brief create the expression
Expression::Expression(ExecutionPlan const* plan, Ast* ast, AstNode* node)
    : _plan(plan), _ast(ast), _node(node), _type(UNPROCESSED), _expressionContext(nullptr) {
  _ast->query()->unPrepareV8Context();
  TRI_ASSERT(_ast != nullptr);
  TRI_ASSERT(_node != nullptr);
}

/// @brief create an expression from VPack
Expression::Expression(ExecutionPlan const* plan, Ast* ast, arangodb::velocypack::Slice const& slice)
    : Expression(plan, ast, new AstNode(ast, slice.get("expression"))) {}

/// @brief destroy the expression
Expression::~Expression() { freeInternals(); }

/// @brief return all variables used in the expression
void Expression::variables(::arangodb::containers::HashSet<Variable const*>& result) const {
  Ast::getReferencedVariables(_node, result);
}

/// @brief execute the expression
AqlValue Expression::execute(transaction::Methods* trx, ExpressionContext* ctx,
                             bool& mustDestroy) {
  buildExpression(trx);

  TRI_ASSERT(_type != UNPROCESSED);
      
  _expressionContext = ctx;

  // and execute
  switch (_type) {
    case JSON: {
      mustDestroy = false;
      TRI_ASSERT(_data != nullptr);
      return AqlValue(_data);
    }

    case SIMPLE: {
      return executeSimpleExpression(_node, trx, mustDestroy, true);
    }

    case ATTRIBUTE_ACCESS: {
      TRI_ASSERT(_accessor != nullptr);
      auto resolver = trx->resolver();
      TRI_ASSERT(resolver != nullptr);
      return _accessor->get(*resolver, ctx, mustDestroy);
    }

    case UNPROCESSED: {
      // fall-through to exception
    }
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "invalid expression type");
}

/// @brief replace variables in the expression with other variables
void Expression::replaceVariables(std::unordered_map<VariableId, Variable const*> const& replacements) {
  _node = _ast->clone(_node);
  TRI_ASSERT(_node != nullptr);

  _node = _ast->replaceVariables(const_cast<AstNode*>(_node), replacements);

  if (_type == ATTRIBUTE_ACCESS && _accessor != nullptr) {
    _accessor->replaceVariable(replacements);
  } else {
    freeInternals();
  }
}

/// @brief replace a variable reference in the expression with another
/// expression (e.g. inserting c = `a + b` into expression `c + 1` so the latter
/// becomes `a + b + 1`
void Expression::replaceVariableReference(Variable const* variable, AstNode const* node) {
  _node = _ast->clone(_node);
  TRI_ASSERT(_node != nullptr);

  _node = _ast->replaceVariableReference(const_cast<AstNode*>(_node), variable, node);
  invalidateAfterReplacements();
}

void Expression::replaceAttributeAccess(Variable const* variable,
                                        std::vector<std::string> const& attribute) {
  _node = _ast->clone(_node);
  TRI_ASSERT(_node != nullptr);

  _node = _ast->replaceAttributeAccess(const_cast<AstNode*>(_node), variable, attribute);
  invalidateAfterReplacements();
}

/// @brief free the internal data structures
void Expression::freeInternals() noexcept {
  switch (_type) {
    case JSON:
      delete[] _data;
      _data = nullptr;
      break;

    case ATTRIBUTE_ACCESS: {
      delete _accessor;
      _accessor = nullptr;
      break;
    }

    case SIMPLE:
    case UNPROCESSED: {
      // nothing to do
      break;
    }
  }
}

/// @brief reset internal attributes after variables in the expression were
/// changed
void Expression::invalidateAfterReplacements() {
  if (_type == ATTRIBUTE_ACCESS || _type == SIMPLE) {
    freeInternals();
    // must even set back the expression type so the expression will be analyzed
    // again
    _type = UNPROCESSED;
    _node->clearFlagsRecursive();  // recursively delete the node's flags
  }

  const_cast<AstNode*>(_node)->clearFlags();
}

/// @brief invalidates an expression
/// this only has an effect for V8-based functions, which need to be created,
/// used and destroyed in the same context. when a V8 function is used across
/// multiple V8 contexts, it must be invalidated in between
void Expression::invalidate() {
  // context may change next time, so "prepare for re-preparation"
  _ast->query()->unPrepareV8Context();
}

/// @brief find a value in an AQL array node
/// this performs either a binary search (if the node is sorted) or a
/// linear search (if the node is not sorted)
bool Expression::findInArray(AqlValue const& left, AqlValue const& right,
                             transaction::Methods* trx, AstNode const* node) const {
  TRI_ASSERT(right.isArray());

  size_t const n = right.length();

  if (n >= AstNode::SortNumberThreshold &&
      (node->getMember(1)->isSorted() ||
       ((node->type == NODE_TYPE_OPERATOR_BINARY_IN || node->type == NODE_TYPE_OPERATOR_BINARY_NIN) &&
        node->getBoolValue()))) {
    // node values are sorted. can use binary search
    size_t l = 0;
    size_t r = n - 1;

    while (true) {
      // determine midpoint
      size_t m = l + ((r - l) / 2);

      bool localMustDestroy;
      AqlValue a = right.at(m, localMustDestroy, false);
      AqlValueGuard guard(a, localMustDestroy);

      int compareResult = AqlValue::Compare(trx, left, a, true);

      if (compareResult == 0) {
        // item found in the list
        return true;
      }

      if (compareResult < 0) {
        if (m == 0) {
          // not found
          return false;
        }
        r = m - 1;
      } else {
        l = m + 1;
      }
      if (r < l) {
        return false;
      }
    }
  }

  // if right operand of IN/NOT IN is a range, we can use an optimized search
  if (right.isRange()) {
    // but only if left operand is a number
    if (!left.isNumber()) {
      // a non-number will never be contained in the range
      return false;
    }

    // check if conversion to int64 would be lossy
    int64_t value = left.toInt64();
    if (left.toDouble() == static_cast<double>(value)) {
      // no loss
      Range const* r = right.range();
      TRI_ASSERT(r != nullptr);

      return r->isIn(value);
    }
    // fall-through to linear search
  }

  // use linear search
  for (size_t i = 0; i < n; ++i) {
    bool mustDestroy;
    AqlValue a = right.at(i, mustDestroy, false);
    AqlValueGuard guard(a, mustDestroy);

    int compareResult = AqlValue::Compare(trx, left, a, false);

    if (compareResult == 0) {
      // item found in the list
      return true;
    }
  }

  return false;
}

/// @brief analyze the expression (determine its type etc.)
void Expression::initExpression() {
  TRI_ASSERT(_type == UNPROCESSED);

  if (_node->isConstant()) {
    // expression is a constant value
    _data = nullptr;
    _type = JSON;
    return;
  }

  // expression is a simple expression
  _type = SIMPLE;

  if (_node->type != NODE_TYPE_ATTRIBUTE_ACCESS) {
    return;
  }

  // optimization for attribute accesses
  TRI_ASSERT(_node->numMembers() == 1);
  auto member = _node->getMemberUnchecked(0);
  std::vector<std::string> parts{_node->getString()};

  while (member->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    parts.insert(parts.begin(), member->getString());
    member = member->getMemberUnchecked(0);
  }

  if (member->type != NODE_TYPE_REFERENCE) {
    return;
  }
  auto v = static_cast<Variable const*>(member->getData());

  bool dataIsFromCollection = false;
  if (_plan != nullptr) {
    // check if the variable we are referring to is set by
    // a collection enumeration/index enumeration
    auto setter = _plan->getVarSetBy(v->id);
    if (setter != nullptr && (setter->getType() == ExecutionNode::INDEX ||
                              setter->getType() == ExecutionNode::ENUMERATE_COLLECTION)) {
      // it is
      dataIsFromCollection = true;
    }
  }

  // specialize the simple expression into an attribute accessor
  _accessor = AttributeAccessor::create(std::move(parts), v, dataIsFromCollection);
  _type = ATTRIBUTE_ACCESS;
}

/// @brief build the expression
void Expression::buildExpression(transaction::Methods* trx) {
  if (_type == UNPROCESSED) {
    initExpression();
  }

  if (_type == JSON && _data == nullptr) {
    // generate a constant value
    transaction::BuilderLeaser builder(trx);
    _node->toVelocyPackValue(*builder.get());

    _data = new uint8_t[static_cast<size_t>(builder->size())];
    memcpy(_data, builder->data(), static_cast<size_t>(builder->size()));
  }
}

/// @brief execute an expression of type SIMPLE, the convention is that
/// the resulting AqlValue will be destroyed outside eventually
AqlValue Expression::executeSimpleExpression(AstNode const* node, transaction::Methods* trx,
                                             bool& mustDestroy, bool doCopy) {
  switch (node->type) {
    case NODE_TYPE_ATTRIBUTE_ACCESS:
      return executeSimpleExpressionAttributeAccess(node, trx, mustDestroy, doCopy);
    case NODE_TYPE_INDEXED_ACCESS:
      return executeSimpleExpressionIndexedAccess(node, trx, mustDestroy, doCopy);
    case NODE_TYPE_ARRAY:
      return executeSimpleExpressionArray(node, trx, mustDestroy);
    case NODE_TYPE_OBJECT:
      return executeSimpleExpressionObject(node, trx, mustDestroy);
    case NODE_TYPE_VALUE:
      return executeSimpleExpressionValue(node, trx, mustDestroy);
    case NODE_TYPE_REFERENCE:
      return executeSimpleExpressionReference(node, trx, mustDestroy, doCopy);
    case NODE_TYPE_FCALL:
      return executeSimpleExpressionFCall(node, trx, mustDestroy);
    case NODE_TYPE_FCALL_USER:
      return executeSimpleExpressionFCallJS(node, trx, mustDestroy);
    case NODE_TYPE_RANGE:
      return executeSimpleExpressionRange(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_UNARY_NOT:
      return executeSimpleExpressionNot(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_UNARY_PLUS:
      return executeSimpleExpressionPlus(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_UNARY_MINUS:
      return executeSimpleExpressionMinus(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_BINARY_AND:
      return executeSimpleExpressionAnd(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_BINARY_OR:
      return executeSimpleExpressionOr(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_BINARY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_NE:
    case NODE_TYPE_OPERATOR_BINARY_LT:
    case NODE_TYPE_OPERATOR_BINARY_LE:
    case NODE_TYPE_OPERATOR_BINARY_GT:
    case NODE_TYPE_OPERATOR_BINARY_GE:
    case NODE_TYPE_OPERATOR_BINARY_IN:
    case NODE_TYPE_OPERATOR_BINARY_NIN:
      return executeSimpleExpressionComparison(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_IN:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN:
      return executeSimpleExpressionArrayComparison(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_TERNARY:
      return executeSimpleExpressionTernary(node, trx, mustDestroy);
    case NODE_TYPE_EXPANSION:
      return executeSimpleExpressionExpansion(node, trx, mustDestroy);
    case NODE_TYPE_ITERATOR:
      return executeSimpleExpressionIterator(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_BINARY_PLUS:
    case NODE_TYPE_OPERATOR_BINARY_MINUS:
    case NODE_TYPE_OPERATOR_BINARY_TIMES:
    case NODE_TYPE_OPERATOR_BINARY_DIV:
    case NODE_TYPE_OPERATOR_BINARY_MOD:
      return executeSimpleExpressionArithmetic(node, trx, mustDestroy);
    case NODE_TYPE_OPERATOR_NARY_AND:
    case NODE_TYPE_OPERATOR_NARY_OR:
      return executeSimpleExpressionNaryAndOr(node, trx, mustDestroy);
    case NODE_TYPE_COLLECTION:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_NOT_IMPLEMENTED,
          "node type 'collection' is not supported in ArangoDB 3.4");
    case NODE_TYPE_VIEW:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_NOT_IMPLEMENTED,
          "node type 'view' is not supported in ArangoDB 3.4");

    default:
      std::string msg("unhandled type '");
      msg.append(node->getTypeString());
      msg.append("' in executeSimpleExpression()");
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, msg);
  }
}

/// @brief check whether this is an attribute access of any degree (e.g. a.b,
/// a.b.c, ...)
bool Expression::isAttributeAccess() const {
  return _node->isAttributeAccessForVariable();
}

/// @brief check whether this is a reference access
bool Expression::isReference() const {
  return (_node->type == arangodb::aql::NODE_TYPE_REFERENCE);
}

/// @brief check whether this is a constant node
bool Expression::isConstant() const { return _node->isConstant(); }

/// @brief stringify an expression
/// note that currently stringification is only supported for certain node types
void Expression::stringify(arangodb::basics::StringBuffer* buffer) const {
  _node->stringify(buffer, true, false);
}

/// @brief stringify an expression
/// note that currently stringification is only supported for certain node types
void Expression::stringifyIfNotTooLong(arangodb::basics::StringBuffer* buffer) const {
  _node->stringify(buffer, true, true);
}

/// @brief execute an expression of type SIMPLE with ATTRIBUTE ACCESS
/// always creates a copy
AqlValue Expression::executeSimpleExpressionAttributeAccess(AstNode const* node,
                                                            transaction::Methods* trx,
                                                            bool& mustDestroy, bool doCopy) {
  // object lookup, e.g. users.name
  TRI_ASSERT(node->numMembers() == 1);

  auto member = node->getMemberUnchecked(0);

  bool localMustDestroy;
  AqlValue result = executeSimpleExpression(member, trx, localMustDestroy, false);
  AqlValueGuard guard(result, localMustDestroy);
  auto resolver = trx->resolver();
  TRI_ASSERT(resolver != nullptr);

  return result.get(
      *resolver, 
      arangodb::velocypack::StringRef(static_cast<char const*>(node->getData()), node->getStringLength()), 
      mustDestroy, 
      true
  );
}

/// @brief execute an expression of type SIMPLE with INDEXED ACCESS
AqlValue Expression::executeSimpleExpressionIndexedAccess(AstNode const* node,
                                                          transaction::Methods* trx,
                                                          bool& mustDestroy, bool doCopy) {
  // array lookup, e.g. users[0]
  // note: it depends on the type of the value whether an array lookup or an
  // object lookup is performed
  // for example, if the value is an object, then its elements might be accessed
  // like this:
  // users['name'] or even users['0'] (as '0' is a valid attribute name, too)
  // if the value is an array, then string indexes might also be used and will
  // be converted to integers, e.g.
  // users['0'] is the same as users[0], users['-2'] is the same as users[-2]
  // etc.
  TRI_ASSERT(node->numMembers() == 2);

  auto member = node->getMemberUnchecked(0);
  auto index = node->getMemberUnchecked(1);

  mustDestroy = false;
  AqlValue result = executeSimpleExpression(member, trx, mustDestroy, false);

  AqlValueGuard guard(result, mustDestroy);

  if (result.isArray()) {
    AqlValue indexResult = executeSimpleExpression(index, trx, mustDestroy, false);

    AqlValueGuard guard(indexResult, mustDestroy);

    if (indexResult.isNumber()) {
      return result.at(indexResult.toInt64(), mustDestroy, true);
    }

    if (indexResult.isString()) {
      VPackSlice s = indexResult.slice();
      TRI_ASSERT(s.isString());
      VPackValueLength l;
      char const* p = s.getString(l);

      bool valid;
      int64_t position = NumberUtils::atoi<int64_t>(p, p + l, valid);
      if (valid) {
        return result.at(position, mustDestroy, true);
      }
      // no number found.
    }

    // fall-through to returning null
  } else if (result.isObject()) {
    AqlValue indexResult = executeSimpleExpression(index, trx, mustDestroy, false);

    AqlValueGuard guard(indexResult, mustDestroy);

    if (indexResult.isNumber()) {
      std::string const indexString = std::to_string(indexResult.toInt64());
      auto resolver = trx->resolver();
      TRI_ASSERT(resolver != nullptr);
      return result.get(*resolver, indexString, mustDestroy, true);
    }

    if (indexResult.isString()) {
      VPackValueLength l;
      char const* p = indexResult.slice().getStringUnchecked(l);
      auto resolver = trx->resolver();
      TRI_ASSERT(resolver != nullptr);
      return result.get(*resolver, arangodb::velocypack::StringRef(p, l), mustDestroy, true);
    }

    // fall-through to returning null
  }

  return AqlValue(AqlValueHintNull());
}

/// @brief execute an expression of type SIMPLE with ARRAY
AqlValue Expression::executeSimpleExpressionArray(AstNode const* node,
                                                  transaction::Methods* trx,
                                                  bool& mustDestroy) {
  mustDestroy = false;
  if (node->isConstant()) {
    // this will not create a copy
    return AqlValue(node->computeValue().begin());
  }

  size_t const n = node->numMembers();

  if (n == 0) {
    return AqlValue(AqlValueHintEmptyArray());
  }

  transaction::BuilderLeaser builder(trx);
  builder->openArray();

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMemberUnchecked(i);
    bool localMustDestroy = false;
    AqlValue result = executeSimpleExpression(member, trx, localMustDestroy, false);
    AqlValueGuard guard(result, localMustDestroy);
    result.toVelocyPack(trx, *builder.get(), false);
  }

  builder->close();
  mustDestroy = true;  // AqlValue contains builder contains dynamic data
  return AqlValue(builder.get());
}

/// @brief execute an expression of type SIMPLE with OBJECT
AqlValue Expression::executeSimpleExpressionObject(AstNode const* node,
                                                   transaction::Methods* trx,
                                                   bool& mustDestroy) {
  mustDestroy = false;
  if (node->isConstant()) {
    // this will not create a copy
    return AqlValue(node->computeValue().begin());
  }

  size_t const n = node->numMembers();

  if (n == 0) {
    return AqlValue(AqlValueHintEmptyObject());
  }

  // unordered set for tracking unique object keys
  std::unordered_set<std::string> keys;
  bool const mustCheckUniqueness = node->mustCheckUniqueness();

  transaction::BuilderLeaser builder(trx);
  builder->openObject();

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMemberUnchecked(i);

    // process attribute key, taking into account duplicates
    if (member->type == NODE_TYPE_CALCULATED_OBJECT_ELEMENT) {
      bool localMustDestroy;
      AqlValue result =
          executeSimpleExpression(member->getMember(0), trx, localMustDestroy, false);
      AqlValueGuard guard(result, localMustDestroy);

      // make sure key is a string, and convert it if not
      transaction::StringBufferLeaser buffer(trx);
      arangodb::basics::VPackStringBufferAdapter adapter(buffer->stringBuffer());

      AqlValueMaterializer materializer(trx);
      VPackSlice slice = materializer.slice(result, false);

      Functions::Stringify(trx, adapter, slice);

      if (mustCheckUniqueness) {
        std::string key(buffer->begin(), buffer->length());

        // prevent duplicate keys from being used
        auto it = keys.find(key);

        if (it != keys.end()) {
          // duplicate key
          continue;
        }

        // unique key
        builder->add(VPackValue(key));
        if (i != n - 1) {
          // track usage of key
          keys.emplace(std::move(key));
        }
      } else {
        builder->add(VPackValuePair(buffer->begin(), buffer->length(), VPackValueType::String));
      }

      // value
      member = member->getMember(1);
    } else {
      TRI_ASSERT(member->type == NODE_TYPE_OBJECT_ELEMENT);

      if (mustCheckUniqueness) {
        std::string key(member->getString());

        // track each individual object key
        auto it = keys.find(key);

        if (it != keys.end()) {
          // duplicate key
          continue;
        }

        // unique key
        builder->add(VPackValue(key));
        if (i != n - 1) {
          // track usage of key
          keys.emplace(std::move(key));
        }
      } else {
        builder->add(VPackValuePair(member->getStringValue(),
                                    member->getStringLength(), VPackValueType::String));
      }

      // value
      member = member->getMember(0);
    }

    // add the attribute value
    bool localMustDestroy;
    AqlValue result = executeSimpleExpression(member, trx, localMustDestroy, false);
    AqlValueGuard guard(result, localMustDestroy);
    result.toVelocyPack(trx, *builder.get(), false);
  }

  builder->close();

  mustDestroy = true;  // AqlValue contains builder contains dynamic data

  return AqlValue(*builder.get());
}

/// @brief execute an expression of type SIMPLE with VALUE
AqlValue Expression::executeSimpleExpressionValue(AstNode const* node,
                                                  transaction::Methods* trx,
                                                  bool& mustDestroy) {
  // this will not create a copy
  mustDestroy = false;
  return AqlValue(node->computeValue().begin());
}

/// @brief execute an expression of type SIMPLE with REFERENCE
AqlValue Expression::executeSimpleExpressionReference(AstNode const* node,
                                                      transaction::Methods* trx,
                                                      bool& mustDestroy, bool doCopy) {
  mustDestroy = false;
  auto v = static_cast<Variable const*>(node->getData());
  TRI_ASSERT(v != nullptr);

  if (!_variables.empty()) {
    auto it = _variables.find(v);

    if (it != _variables.end()) {
      // copy the slice we found
      mustDestroy = true;
      return AqlValue((*it).second);
    }
  }
  return _expressionContext->getVariableValue(v, doCopy, mustDestroy);
}

/// @brief execute an expression of type SIMPLE with RANGE
AqlValue Expression::executeSimpleExpressionRange(AstNode const* node,
                                                  transaction::Methods* trx,
                                                  bool& mustDestroy) {
  auto low = node->getMember(0);
  auto high = node->getMember(1);
  mustDestroy = false;

  AqlValue resultLow = executeSimpleExpression(low, trx, mustDestroy, false);

  AqlValueGuard guardLow(resultLow, mustDestroy);

  AqlValue resultHigh = executeSimpleExpression(high, trx, mustDestroy, false);

  AqlValueGuard guardHigh(resultHigh, mustDestroy);

  mustDestroy = true; // as we're creating a new range object
  return AqlValue(resultLow.toInt64(), resultHigh.toInt64());
}

/// @brief execute an expression of type SIMPLE with FCALL, dispatcher
AqlValue Expression::executeSimpleExpressionFCall(AstNode const* node,
                                                  transaction::Methods* trx,
                                                  bool& mustDestroy) {
  // only some functions have C++ handlers
  // check that the called function actually has one
  auto func = static_cast<Function*>(node->getData());
  if (func->implementation != nullptr) {
    return executeSimpleExpressionFCallCxx(node, trx, mustDestroy);
  }
  return executeSimpleExpressionFCallJS(node, trx, mustDestroy);
}

/// @brief execute an expression of type SIMPLE with FCALL, CXX version
AqlValue Expression::executeSimpleExpressionFCallCxx(AstNode const* node,
                                                     transaction::Methods* trx,
                                                     bool& mustDestroy) {
  mustDestroy = false;
  auto func = static_cast<Function*>(node->getData());
  TRI_ASSERT(func->implementation != nullptr);

  auto member = node->getMemberUnchecked(0);
  TRI_ASSERT(member->type == NODE_TYPE_ARRAY);

  struct FunctionParameters {
    // use stack-based allocation for the first few function call
    // parameters. this saves a few heap allocations per function
    // call invocation
    ::arangodb::containers::SmallVector<AqlValue>::allocator_type::arena_type arena;
    VPackFunctionParameters parameters{arena};

    // same here
    ::arangodb::containers::SmallVector<uint64_t>::allocator_type::arena_type arena2;
    ::arangodb::containers::SmallVector<uint64_t> destroyParameters{arena2};

    explicit FunctionParameters(size_t n) {
      parameters.reserve(n);
      destroyParameters.reserve(n);
    }

    ~FunctionParameters() {
      for (size_t i = 0; i < destroyParameters.size(); ++i) {
        if (destroyParameters[i]) {
          parameters[i].destroy();
        }
      }
    }
  };

  size_t const n = member->numMembers();

  FunctionParameters params(n);

  for (size_t i = 0; i < n; ++i) {
    auto arg = member->getMemberUnchecked(i);

    if (arg->type == NODE_TYPE_COLLECTION) {
      params.parameters.emplace_back(arg->getStringValue(), arg->getStringLength());
      params.destroyParameters.push_back(1);
    } else {
      bool localMustDestroy;
      params.parameters.emplace_back(
          executeSimpleExpression(arg, trx, localMustDestroy, false));
      params.destroyParameters.push_back(localMustDestroy ? 1 : 0);
    }
  }

  TRI_ASSERT(params.parameters.size() == params.destroyParameters.size());
  TRI_ASSERT(params.parameters.size() == n);

  AqlValue a = func->implementation(_expressionContext, trx, params.parameters);
  mustDestroy = true;  // function result is always dynamic

  return a;
}

AqlValue Expression::invokeV8Function(ExpressionContext* expressionContext,
                                      transaction::Methods* trx, std::string const& jsName,
                                      std::string const& ucInvokeFN, char const* AFN,
                                      bool rethrowV8Exception, size_t callArgs,
                                      v8::Handle<v8::Value>* args, bool& mustDestroy) {
  ISOLATE;
  auto current = isolate->GetCurrentContext()->Global();

  v8::Handle<v8::Value> module =
      current->Get(TRI_V8_ASCII_STRING(isolate, "_AQL"));
  if (module.IsEmpty() || !module->IsObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "unable to find global _AQL module");
  }

  v8::Handle<v8::Value> function =
      v8::Handle<v8::Object>::Cast(module)->Get(TRI_V8_STD_STRING(isolate, jsName));
  if (function.IsEmpty() || !function->IsFunction()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        std::string("unable to find AQL function '") + jsName + "'");
  }

  // actually call the V8 function
  v8::TryCatch tryCatch(isolate);
  v8::Handle<v8::Value> result =
      v8::Handle<v8::Function>::Cast(function)->Call(current, static_cast<int>(callArgs), args);

  try {
    V8Executor::HandleV8Error(tryCatch, result, nullptr, false);
  } catch (arangodb::basics::Exception const& ex) {
    if (rethrowV8Exception || ex.code() == TRI_ERROR_QUERY_FUNCTION_NOT_FOUND) {
      throw;
    }
    std::string message("while invoking '");
    message += ucInvokeFN + "' via '" + AFN + "': " + ex.message();
    expressionContext->registerWarning(ex.code(), message.c_str());
    return AqlValue(AqlValueHintNull());
  }
  if (result.IsEmpty() || result->IsUndefined()) {
    return AqlValue(AqlValueHintNull());
  }

  transaction::BuilderLeaser builder(trx);

  int res = TRI_V8ToVPack(isolate, *builder.get(), result, false);

  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(res);
  }

  mustDestroy = true;  // builder = dynamic data
  return AqlValue(builder.get());
}

/// @brief execute an expression of type SIMPLE, JavaScript variant
AqlValue Expression::executeSimpleExpressionFCallJS(AstNode const* node,
                                                    transaction::Methods* trx,
                                                    bool& mustDestroy) {
  auto member = node->getMemberUnchecked(0);
  TRI_ASSERT(member->type == NODE_TYPE_ARRAY);

  mustDestroy = false;

  {
    ISOLATE;
    TRI_ASSERT(isolate != nullptr);
    v8::HandleScope scope(isolate);                                   \
    _ast->query()->prepareV8Context();

    std::string jsName;
    size_t const n = static_cast<int>(member->numMembers());
    size_t callArgs = (node->type == NODE_TYPE_FCALL_USER ? 2 : n);
    auto args = std::make_unique<v8::Handle<v8::Value>[]>(callArgs);

    if (node->type == NODE_TYPE_FCALL_USER) {
      // a call to a user-defined function
      jsName = "FCALL_USER";
      v8::Handle<v8::Array> params = v8::Array::New(isolate, static_cast<int>(n));

      for (size_t i = 0; i < n; ++i) {
        auto arg = member->getMemberUnchecked(i);

        bool localMustDestroy;
        AqlValue a = executeSimpleExpression(arg, trx, localMustDestroy, false);
        AqlValueGuard guard(a, localMustDestroy);

        params->Set(static_cast<uint32_t>(i), a.toV8(isolate, trx));
      }

      // function name
      args[0] = TRI_V8_STD_STRING(isolate, node->getString());
      // call parameters
      args[1] = params;
      // args[2] will be null
    } else {
      // a call to a built-in V8 function
      auto func = static_cast<Function*>(node->getData());
      jsName = "AQL_" + func->name;

      for (size_t i = 0; i < n; ++i) {
        auto arg = member->getMemberUnchecked(i);

        if (arg->type == NODE_TYPE_COLLECTION) {
          // parameter conversion for NODE_TYPE_COLLECTION here
          args[i] = TRI_V8_ASCII_PAIR_STRING(isolate, arg->getStringValue(),
                                             arg->getStringLength());
        } else {
          bool localMustDestroy;
          AqlValue a = executeSimpleExpression(arg, trx, localMustDestroy, false);
          AqlValueGuard guard(a, localMustDestroy);

          args[i] = a.toV8(isolate, trx);
        }
      }
    }

    return invokeV8Function(_expressionContext, trx, jsName, "", "", true,
                            callArgs, args.get(), mustDestroy);
  }
}

/// @brief execute an expression of type SIMPLE with NOT
AqlValue Expression::executeSimpleExpressionNot(AstNode const* node,
                                                transaction::Methods* trx, bool& mustDestroy) {
  mustDestroy = false;
  AqlValue operand = executeSimpleExpression(node->getMember(0), trx, mustDestroy, false);

  AqlValueGuard guard(operand, mustDestroy);
  bool const operandIsTrue = operand.toBoolean();

  mustDestroy = false;  // only a boolean
  return AqlValue(AqlValueHintBool(!operandIsTrue));
}

/// @brief execute an expression of type SIMPLE with +
AqlValue Expression::executeSimpleExpressionPlus(AstNode const* node,
                                                 transaction::Methods* trx,
                                                 bool& mustDestroy) {
  mustDestroy = false;
  AqlValue operand = executeSimpleExpression(node->getMember(0), trx, mustDestroy, false);

  AqlValueGuard guard(operand, mustDestroy);

  if (operand.isNumber()) {
    VPackSlice const s = operand.slice();

    if (s.isSmallInt() || s.isInt()) {
      // can use int64
      return AqlValue(AqlValueHintInt(s.getNumber<int64_t>()));
    } else if (s.isUInt()) {
      // can use uint64
      return AqlValue(AqlValueHintUInt(s.getUInt()));
    }
    // fallthrouh intentional
  }

  // use a double value for all other cases
  bool failed = false;
  double value = operand.toDouble(failed);

  if (failed) {
    value = 0.0;
  }

  return AqlValue(AqlValueHintDouble(+value));
}

/// @brief execute an expression of type SIMPLE with -
AqlValue Expression::executeSimpleExpressionMinus(AstNode const* node,
                                                  transaction::Methods* trx,
                                                  bool& mustDestroy) {
  mustDestroy = false;
  AqlValue operand = executeSimpleExpression(node->getMember(0), trx, mustDestroy, false);

  AqlValueGuard guard(operand, mustDestroy);

  if (operand.isNumber()) {
    VPackSlice const s = operand.slice();
    if (s.isSmallInt()) {
      // can use int64
      return AqlValue(AqlValueHintInt(-s.getNumber<int64_t>()));
    } else if (s.isInt()) {
      int64_t v = s.getNumber<int64_t>();
      if (v != INT64_MIN) {
        // can use int64
        return AqlValue(AqlValueHintInt(-v));
      }
    } else if (s.isUInt()) {
      uint64_t v = s.getNumber<uint64_t>();
      if (v <= uint64_t(INT64_MAX)) {
        // can use int64 too
        int64_t v = s.getNumber<int64_t>();
        return AqlValue(AqlValueHintInt(-v));
      }
    }
    // fallthrough intentional
  }

  bool failed = false;
  double value = operand.toDouble(failed);

  if (failed) {
    value = 0.0;
  }

  return AqlValue(AqlValueHintDouble(-value));
}

/// @brief execute an expression of type SIMPLE with AND
AqlValue Expression::executeSimpleExpressionAnd(AstNode const* node,
                                                transaction::Methods* trx, bool& mustDestroy) {
  AqlValue left =
      executeSimpleExpression(node->getMemberUnchecked(0), trx, mustDestroy, true);

  if (left.toBoolean()) {
    // left is true => return right
    if (mustDestroy) {
      left.destroy();
    }
    return executeSimpleExpression(node->getMemberUnchecked(1), trx, mustDestroy, true);
  }

  // left is false, return left
  return left;
}

/// @brief execute an expression of type SIMPLE with OR
AqlValue Expression::executeSimpleExpressionOr(AstNode const* node,
                                               transaction::Methods* trx, bool& mustDestroy) {
  AqlValue left =
      executeSimpleExpression(node->getMemberUnchecked(0), trx, mustDestroy, true);

  if (left.toBoolean()) {
    // left is true => return left
    return left;
  }

  // left is false => return right
  if (mustDestroy) {
    left.destroy();
  }
  return executeSimpleExpression(node->getMemberUnchecked(1), trx, mustDestroy, true);
}

/// @brief execute an expression of type SIMPLE with AND or OR
AqlValue Expression::executeSimpleExpressionNaryAndOr(AstNode const* node,
                                                      transaction::Methods* trx,
                                                      bool& mustDestroy) {
  mustDestroy = false;
  size_t count = node->numMembers();
  if (count == 0) {
    // There is nothing to evaluate. So this is always true
    return AqlValue(AqlValueHintBool(true));
  }

  // AND
  if (node->type == NODE_TYPE_OPERATOR_NARY_AND) {
    for (size_t i = 0; i < count; ++i) {
      bool localMustDestroy = false;
      AqlValue check = executeSimpleExpression(node->getMemberUnchecked(i), trx,
                                               localMustDestroy, false);
      bool result = check.toBoolean();

      if (localMustDestroy) {
        check.destroy();
      }

      if (!result) {
        // we are allowed to return early here, because this is only called
        // in the context of index lookups
        return AqlValue(AqlValueHintBool(false));
      }
    }
    return AqlValue(AqlValueHintBool(true));
  }

  // OR
  for (size_t i = 0; i < count; ++i) {
    bool localMustDestroy = false;
    AqlValue check = executeSimpleExpression(node->getMemberUnchecked(i), trx,
                                             localMustDestroy, true);
    bool result = check.toBoolean();

    if (localMustDestroy) {
      check.destroy();
    }

    if (result) {
      // we are allowed to return early here, because this is only called
      // in the context of index lookups
      return AqlValue(AqlValueHintBool(true));
    }
  }

  // anything else... we shouldn't get here
  TRI_ASSERT(false);
  return AqlValue(AqlValueHintBool(false));
}

/// @brief execute an expression of type SIMPLE with COMPARISON
AqlValue Expression::executeSimpleExpressionComparison(AstNode const* node,
                                                       transaction::Methods* trx,
                                                       bool& mustDestroy) {
  AqlValue left =
      executeSimpleExpression(node->getMemberUnchecked(0), trx, mustDestroy, false);
  AqlValueGuard guardLeft(left, mustDestroy);

  AqlValue right =
      executeSimpleExpression(node->getMemberUnchecked(1), trx, mustDestroy, false);
  AqlValueGuard guardRight(right, mustDestroy);

  mustDestroy = false;  // we're returning a boolean only

  if (node->type == NODE_TYPE_OPERATOR_BINARY_IN || node->type == NODE_TYPE_OPERATOR_BINARY_NIN) {
    // IN and NOT IN
    if (!right.isArray()) {
      // right operand must be an array, otherwise we return false
      // do not throw, but return "false" instead
      return AqlValue(AqlValueHintBool(false));
    }

    bool result = findInArray(left, right, trx, node);

    if (node->type == NODE_TYPE_OPERATOR_BINARY_NIN) {
      // revert the result in case of a NOT IN
      result = !result;
    }

    return AqlValue(AqlValueHintBool(result));
  }

  // all other comparison operators...

  // for equality and non-equality we can use a binary comparison
  bool compareUtf8 = (node->type != NODE_TYPE_OPERATOR_BINARY_EQ &&
                      node->type != NODE_TYPE_OPERATOR_BINARY_NE);

  int compareResult = AqlValue::Compare(trx, left, right, compareUtf8);

  switch (node->type) {
    case NODE_TYPE_OPERATOR_BINARY_EQ:
      return AqlValue(AqlValueHintBool(compareResult == 0));
    case NODE_TYPE_OPERATOR_BINARY_NE:
      return AqlValue(AqlValueHintBool(compareResult != 0));
    case NODE_TYPE_OPERATOR_BINARY_LT:
      return AqlValue(AqlValueHintBool(compareResult < 0));
    case NODE_TYPE_OPERATOR_BINARY_LE:
      return AqlValue(AqlValueHintBool(compareResult <= 0));
    case NODE_TYPE_OPERATOR_BINARY_GT:
      return AqlValue(AqlValueHintBool(compareResult > 0));
    case NODE_TYPE_OPERATOR_BINARY_GE:
      return AqlValue(AqlValueHintBool(compareResult >= 0));
    default:
      std::string msg("unhandled type '");
      msg.append(node->getTypeString());
      msg.append("' in executeSimpleExpression()");
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, msg);
  }
}

/// @brief execute an expression of type SIMPLE with ARRAY COMPARISON
AqlValue Expression::executeSimpleExpressionArrayComparison(AstNode const* node,
                                                            transaction::Methods* trx,
                                                            bool& mustDestroy) {
  AqlValue left = executeSimpleExpression(node->getMember(0), trx, mustDestroy, false);
  AqlValueGuard guardLeft(left, mustDestroy);

  AqlValue right = executeSimpleExpression(node->getMember(1), trx, mustDestroy, false);
  AqlValueGuard guardRight(right, mustDestroy);

  mustDestroy = false;  // we're returning a boolean only

  if (!left.isArray()) {
    // left operand must be an array
    // do not throw, but return "false" instead
    return AqlValue(AqlValueHintBool(false));
  }

  if (node->type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
      node->type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN) {
    // IN and NOT IN
    if (!right.isArray()) {
      // right operand must be an array, otherwise we return false
      // do not throw, but return "false" instead
      return AqlValue(AqlValueHintBool(false));
    }
  }

  size_t const n = left.length();

  if (n == 0) {
    if (Quantifier::IsAllOrNone(node->getMember(2))) {
      // [] ALL ...
      // [] NONE ...
      return AqlValue(AqlValueHintBool(true));
    } else {
      // [] ANY ...
      return AqlValue(AqlValueHintBool(false));
    }
  }

  std::pair<size_t, size_t> requiredMatches =
      Quantifier::RequiredMatches(n, node->getMember(2));

  TRI_ASSERT(requiredMatches.first <= requiredMatches.second);

  // for equality and non-equality we can use a binary comparison
  bool const compareUtf8 = (node->type != NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ &&
                            node->type != NODE_TYPE_OPERATOR_BINARY_ARRAY_NE);

  bool overallResult = true;
  size_t matches = 0;
  size_t numLeft = n;

  for (size_t i = 0; i < n; ++i) {
    bool localMustDestroy;
    AqlValue leftItemValue = left.at(i, localMustDestroy, false);
    AqlValueGuard guard(leftItemValue, localMustDestroy);

    bool result;

    // IN and NOT IN
    if (node->type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
        node->type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN) {
      result = findInArray(leftItemValue, right, trx, node);

      if (node->type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN) {
        // revert the result in case of a NOT IN
        result = !result;
      }
    } else {
      // other operators
      int compareResult = AqlValue::Compare(trx, leftItemValue, right, compareUtf8);

      result = false;
      switch (node->type) {
        case NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ:
          result = (compareResult == 0);
          break;
        case NODE_TYPE_OPERATOR_BINARY_ARRAY_NE:
          result = (compareResult != 0);
          break;
        case NODE_TYPE_OPERATOR_BINARY_ARRAY_LT:
          result = (compareResult < 0);
          break;
        case NODE_TYPE_OPERATOR_BINARY_ARRAY_LE:
          result = (compareResult <= 0);
          break;
        case NODE_TYPE_OPERATOR_BINARY_ARRAY_GT:
          result = (compareResult > 0);
          break;
        case NODE_TYPE_OPERATOR_BINARY_ARRAY_GE:
          result = (compareResult >= 0);
          break;
        default:
          TRI_ASSERT(false);
      }
    }

    --numLeft;

    if (result) {
      ++matches;
      if (matches > requiredMatches.second) {
        // too many matches
        overallResult = false;
        break;
      }
      if (matches >= requiredMatches.first &&
          matches + numLeft <= requiredMatches.second) {
        // enough matches
        overallResult = true;
        break;
      }
    } else {
      if (matches + numLeft < requiredMatches.first) {
        // too few matches
        overallResult = false;
        break;
      }
    }
  }

  TRI_ASSERT(!mustDestroy);
  return AqlValue(AqlValueHintBool(overallResult));
}

/// @brief execute an expression of type SIMPLE with TERNARY
AqlValue Expression::executeSimpleExpressionTernary(AstNode const* node,
                                                    transaction::Methods* trx,
                                                    bool& mustDestroy) {
  AqlValue condition =
      executeSimpleExpression(node->getMember(0), trx, mustDestroy, false);

  AqlValueGuard guardCondition(condition, mustDestroy);

  size_t position;
  if (condition.toBoolean()) {
    // return true part
    position = 1;
  } else {
    // return false part
    position = 2;
  }

  return executeSimpleExpression(node->getMemberUnchecked(position), trx, mustDestroy, true);
}

/// @brief execute an expression of type SIMPLE with EXPANSION
AqlValue Expression::executeSimpleExpressionExpansion(AstNode const* node,
                                                      transaction::Methods* trx,
                                                      bool& mustDestroy) {
  TRI_ASSERT(node->numMembers() == 5);
  mustDestroy = false;

  // LIMIT
  int64_t offset = 0;
  int64_t count = INT64_MAX;

  auto limitNode = node->getMember(3);

  if (limitNode->type != NODE_TYPE_NOP) {
    bool localMustDestroy;
    AqlValue subOffset =
        executeSimpleExpression(limitNode->getMember(0), trx, localMustDestroy, false);
    offset = subOffset.toInt64();
    if (localMustDestroy) {
      subOffset.destroy();
    }

    AqlValue subCount =
        executeSimpleExpression(limitNode->getMember(1), trx, localMustDestroy, false);
    count = subCount.toInt64();
    if (localMustDestroy) {
      subCount.destroy();
    }
  }

  if (offset < 0 || count <= 0) {
    // no items to return... can already stop here
    return AqlValue(AqlValueHintEmptyArray());
  }

  // FILTER
  AstNode const* filterNode = node->getMember(2);

  if (filterNode->type == NODE_TYPE_NOP) {
    filterNode = nullptr;
  } else if (filterNode->isConstant()) {
    if (filterNode->isTrue()) {
      // filter expression is always true
      filterNode = nullptr;
    } else {
      // filter expression is always false
      return AqlValue(AqlValueHintEmptyArray());
    }
  }

  auto iterator = node->getMember(0);
  auto variable = static_cast<Variable*>(iterator->getMember(0)->getData());
  auto levels = node->getIntValue(true);

  AqlValue value;

  if (levels > 1) {
    // flatten value...
    bool localMustDestroy;
    AqlValue a = executeSimpleExpression(node->getMember(0), trx, localMustDestroy, false);

    AqlValueGuard guard(a, localMustDestroy);

    if (!a.isArray()) {
      TRI_ASSERT(!mustDestroy);
      return AqlValue(AqlValueHintEmptyArray());
    }

    VPackBuilder builder;
    builder.openArray();

    // generate a new temporary for the flattened array
    std::function<void(AqlValue const&, int64_t)> flatten = [&](AqlValue const& v,
                                                                int64_t level) {
      if (!v.isArray()) {
        return;
      }

      size_t const n = v.length();
      for (size_t i = 0; i < n; ++i) {
        bool localMustDestroy;
        AqlValue item = v.at(i, localMustDestroy, false);
        AqlValueGuard guard(item, localMustDestroy);

        bool const isArray = item.isArray();

        if (!isArray || level == levels) {
          builder.add(item.slice());
        } else if (isArray && level < levels) {
          flatten(item, level + 1);
        }
      }
    };

    flatten(a, 1);
    builder.close();

    mustDestroy = true;  // builder = dynamic data
    value = AqlValue(builder);
  } else {
    bool localMustDestroy;
    AqlValue a = executeSimpleExpression(node->getMember(0), trx, localMustDestroy, false);

    AqlValueGuard guard(a, localMustDestroy);

    if (!a.isArray()) {
      TRI_ASSERT(!mustDestroy);
      return AqlValue(AqlValueHintEmptyArray());
    }

    mustDestroy = localMustDestroy;  // maybe we need to destroy...
    guard.steal();                   // guard is not responsible anymore
    value = a;
  }

  AqlValueGuard guard(value, mustDestroy);

  // RETURN
  // the default is to return array member unmodified
  AstNode const* projectionNode = node->getMember(1);

  if (node->getMember(4)->type != NODE_TYPE_NOP) {
    // return projection
    projectionNode = node->getMember(4);
  }

  if (filterNode == nullptr && projectionNode->type == NODE_TYPE_REFERENCE &&
      value.isArray() && offset == 0 && count == INT64_MAX) {
    // no filter and no projection... we can return the array as it is
    auto other = static_cast<Variable const*>(projectionNode->getData());

    if (other->id == variable->id) {
      // simplify `v[*]` to just `v` if it's already an array
      mustDestroy = true;
      guard.steal();
      return value;
    }
  }

  VPackBuilder builder;
  builder.openArray();

  size_t const n = value.length();
  for (size_t i = 0; i < n; ++i) {
    bool localMustDestroy;
    AqlValue item = value.at(i, localMustDestroy, false);
    AqlValueGuard guard(item, localMustDestroy);

    AqlValueMaterializer materializer(trx);
    setVariable(variable, materializer.slice(item, false));

    bool takeItem = true;

    if (filterNode != nullptr) {
      // have a filter
      bool localMustDestroy;
      AqlValue sub = executeSimpleExpression(filterNode, trx, localMustDestroy, false);

      takeItem = sub.toBoolean();
      if (localMustDestroy) {
        sub.destroy();
      }
    }

    if (takeItem && offset > 0) {
      // there is an offset in place
      --offset;
      takeItem = false;
    }

    if (takeItem) {
      bool localMustDestroy;
      AqlValue sub = executeSimpleExpression(projectionNode, trx, localMustDestroy, false);
      sub.toVelocyPack(trx, builder, false);
      if (localMustDestroy) {
        sub.destroy();
      }
    }

    clearVariable(variable);

    if (takeItem && count > 0) {
      // number of items to pick was restricted
      if (--count == 0) {
        // done
        break;
      }
    }
  }

  builder.close();
  mustDestroy = true;
  return AqlValue(builder);  // builder = dynamic data
}

/// @brief execute an expression of type SIMPLE with ITERATOR
AqlValue Expression::executeSimpleExpressionIterator(AstNode const* node,
                                                     transaction::Methods* trx,
                                                     bool& mustDestroy) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->numMembers() == 2);

  return executeSimpleExpression(node->getMember(1), trx, mustDestroy, true);
}

/// @brief execute an expression of type SIMPLE with BINARY_* (+, -, * , /, %)
AqlValue Expression::executeSimpleExpressionArithmetic(AstNode const* node,
                                                       transaction::Methods* trx,
                                                       bool& mustDestroy) {
  AqlValue lhs =
      executeSimpleExpression(node->getMemberUnchecked(0), trx, mustDestroy, true);
  AqlValueGuard guardLhs(lhs, mustDestroy);

  AqlValue rhs =
      executeSimpleExpression(node->getMemberUnchecked(1), trx, mustDestroy, true);
  AqlValueGuard guardRhs(rhs, mustDestroy);

  mustDestroy = false;

  bool failed = false;
  double l = lhs.toDouble(failed);

  if (failed) {
    TRI_ASSERT(!mustDestroy);
    l = 0.0;
  }

  double r = rhs.toDouble(failed);

  if (failed) {
    TRI_ASSERT(!mustDestroy);
    r = 0.0;
  }

  if (r == 0.0) {
    if (node->type == NODE_TYPE_OPERATOR_BINARY_DIV || node->type == NODE_TYPE_OPERATOR_BINARY_MOD) {
      // division by zero
      TRI_ASSERT(!mustDestroy);
      std::string msg("in operator ");
      msg.append(node->type == NODE_TYPE_OPERATOR_BINARY_DIV ? "/" : "%");
      msg.append(": ");
      msg.append(TRI_errno_string(TRI_ERROR_QUERY_DIVISION_BY_ZERO));
      _expressionContext->registerWarning(TRI_ERROR_QUERY_DIVISION_BY_ZERO, msg.c_str());
      return AqlValue(AqlValueHintNull());
    }
  }

  mustDestroy = false;
  double result;

  switch (node->type) {
    case NODE_TYPE_OPERATOR_BINARY_PLUS:
      result = l + r;
      break;
    case NODE_TYPE_OPERATOR_BINARY_MINUS:
      result = l - r;
      break;
    case NODE_TYPE_OPERATOR_BINARY_TIMES:
      result = l * r;
      break;
    case NODE_TYPE_OPERATOR_BINARY_DIV:
      result = l / r;
      break;
    case NODE_TYPE_OPERATOR_BINARY_MOD:
      result = fmod(l, r);
      break;
    default:
      return AqlValue(AqlValueHintZero());
  }

  // this will convert NaN, +inf & -inf to null
  return AqlValue(AqlValueHintDouble(result));
}
void Expression::replaceNode(AstNode* node) {
  if (node != _node) {
    _node = node;
    invalidateAfterReplacements();
  }
}
Ast* Expression::ast() const noexcept { return _ast; }
AstNode const* Expression::node() const { return _node; }
AstNode* Expression::nodeForModification() const { return _node; }
bool Expression::canRunOnDBServer() {
  if (_type == UNPROCESSED) {
    initExpression();
  }

  if (_type == JSON) {
    // can always run on DB server
    return true;
  }

  TRI_ASSERT(_type == SIMPLE || _type == ATTRIBUTE_ACCESS);
  TRI_ASSERT(_node != nullptr);
  return _node->canRunOnDBServer();
}

bool Expression::isDeterministic() {
  if (_type == UNPROCESSED) {
    initExpression();
  }

  if (_type == JSON) {
    // always deterministic
    return true;
  }

  TRI_ASSERT(_type == SIMPLE || _type == ATTRIBUTE_ACCESS);
  TRI_ASSERT(_node != nullptr);
  return _node->isDeterministic();
}
bool Expression::willUseV8() {
  if (_type == UNPROCESSED) {
    initExpression();
  }

  if (_type != SIMPLE) {
    return false;
  }

  // only simple expressions can make use of V8
  TRI_ASSERT(_type == SIMPLE);
  TRI_ASSERT(_node != nullptr);
  return _node->willUseV8();
}

std::unique_ptr<Expression> Expression::clone(ExecutionPlan* plan, Ast* ast) {
  // We do not need to copy the _ast, since it is managed by the
  // query object and the memory management of the ASTs
  return std::make_unique<Expression>(plan, ast != nullptr ? ast : _ast, _node);
}

void Expression::toVelocyPack(arangodb::velocypack::Builder& builder, bool verbose) const {
  _node->toVelocyPack(builder, verbose);
}

std::string Expression::typeString() {
  if (_type == UNPROCESSED) {
    initExpression();
  }

  switch (_type) {
    case JSON:
      return "json";
    case SIMPLE:
      return "simple";
    case ATTRIBUTE_ACCESS:
      return "attribute";
    case UNPROCESSED: {
    }
  }
  TRI_ASSERT(false);
  return "unknown";
}
void Expression::setVariable(Variable const* variable, arangodb::velocypack::Slice value) {
  _variables.emplace(variable, value);
}
void Expression::clearVariable(Variable const* variable) { _variables.erase(variable); }
