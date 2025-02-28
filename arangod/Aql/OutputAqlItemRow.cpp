////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "OutputAqlItemRow.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/AqlValue.h"
#include "Aql/ExecutorInfos.h"
#include "Aql/ShadowAqlItemRow.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

OutputAqlItemRow::OutputAqlItemRow(
    SharedAqlItemBlockPtr block,
    std::shared_ptr<std::unordered_set<RegisterId> const> outputRegisters,
    std::shared_ptr<std::unordered_set<RegisterId> const> registersToKeep,
    std::shared_ptr<std::unordered_set<RegisterId> const> registersToClear,
    CopyRowBehavior copyRowBehavior)
    : _block(std::move(block)),
      _baseIndex(0),
      _lastBaseIndex(0),
      _inputRowCopied(false),
      _lastSourceRow{CreateInvalidInputRowHint{}},
      _numValuesWritten(0),
      _doNotCopyInputRow(copyRowBehavior == CopyRowBehavior::DoNotCopyInputRows),
      _outputRegisters(std::move(outputRegisters)),
      _registersToKeep(std::move(registersToKeep)),
      _registersToClear(std::move(registersToClear)),
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      _setBaseIndexNotUsed(true),
#endif
      _allowSourceRowUninitialized(false) {
  TRI_ASSERT(_block != nullptr);
}

template <class ItemRowType>
void OutputAqlItemRow::cloneValueInto(RegisterId registerId, ItemRowType const& sourceRow,
                                      AqlValue const& value) {
  bool mustDestroy = true;
  AqlValue clonedValue = value.clone();
  AqlValueGuard guard{clonedValue, mustDestroy};
  moveValueInto(registerId, sourceRow, guard);
}

template <class ItemRowType>
void OutputAqlItemRow::moveValueInto(RegisterId registerId, ItemRowType const& sourceRow,
                                     AqlValueGuard& guard) {
  TRI_ASSERT(isOutputRegister(registerId));
  // This is already implicitly asserted by isOutputRegister:
  TRI_ASSERT(registerId < getNrRegisters());
  TRI_ASSERT(_numValuesWritten < numRegistersToWrite());
  TRI_ASSERT(block().getValueReference(_baseIndex, registerId).isNone());

  block().setValue(_baseIndex, registerId, guard.value());
  guard.steal();
  _numValuesWritten++;
  // allValuesWritten() must be called only *after* _numValuesWritten was
  // increased.
  if (allValuesWritten()) {
    copyRow(sourceRow);
  }
}

void OutputAqlItemRow::consumeShadowRow(RegisterId registerId,
                                        ShadowAqlItemRow const& sourceRow,
                                        AqlValueGuard& guard) {
  TRI_ASSERT(sourceRow.isRelevant());
  moveValueInto(registerId, sourceRow, guard);
  TRI_ASSERT(produced());
  block().makeDataRow(_baseIndex);
}

bool OutputAqlItemRow::reuseLastStoredValue(RegisterId registerId,
                                            InputAqlItemRow const& sourceRow) {
  TRI_ASSERT(isOutputRegister(registerId));
  if (_lastBaseIndex == _baseIndex) {
    return false;
  }
  // Do not clone the value, we explicitly want to recycle it.
  AqlValue ref = block().getValue(_lastBaseIndex, registerId);
  // The initial row is still responsible
  AqlValueGuard guard{ref, false};
  moveValueInto(registerId, sourceRow, guard);
  return true;
}

template <class ItemRowType>
void OutputAqlItemRow::copyRow(ItemRowType const& sourceRow, bool ignoreMissing) {
  // While violating the following asserted states would do no harm, the
  // implementation as planned should only copy a row after all values have
  // been set, and copyRow should only be called once.
  TRI_ASSERT(!_inputRowCopied);
  // We either have a shadowRow, or we need to ahve all values written
  TRI_ASSERT((std::is_same<ItemRowType, ShadowAqlItemRow>::value) || allValuesWritten());
  if (_inputRowCopied) {
    _lastBaseIndex = _baseIndex;
    return;
  }

  // This may only be set if the input block is the same as the output block,
  // because it is passed through.
  if (_doNotCopyInputRow) {
    TRI_ASSERT(sourceRow.isInitialized());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    TRI_ASSERT(sourceRow.internalBlockIs(_block));
#endif
    _inputRowCopied = true;
    memorizeRow(sourceRow);
    return;
  }

  doCopyRow(sourceRow, ignoreMissing);
}

void OutputAqlItemRow::copyBlockInternalRegister(InputAqlItemRow const& sourceRow,
                                                 RegisterId input, RegisterId output) {
  // This method is only allowed if the block of the input row is the same as
  // the block of the output row!
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(sourceRow.internalBlockIs(_block));
#endif
  TRI_ASSERT(isOutputRegister(output));
  // This is already implicitly asserted by isOutputRegister:
  TRI_ASSERT(output < getNrRegisters());
  TRI_ASSERT(_numValuesWritten < numRegistersToWrite());
  TRI_ASSERT(block().getValueReference(_baseIndex, output).isNone());

  AqlValue const& value = sourceRow.getValue(input);

  block().setValue(_baseIndex, output, value);
  _numValuesWritten++;
  // allValuesWritten() must be called only *after* _numValuesWritten was
  // increased.
  if (allValuesWritten()) {
    copyRow(sourceRow);
  }
}

size_t OutputAqlItemRow::numRowsWritten() const noexcept {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(_setBaseIndexNotUsed);
#endif
  // If the current line was fully written, the number of fully written rows
  // is the index plus one.
  if (produced()) {
    return _baseIndex + 1;
  }

  // If the current line was not fully written, the last one was, so the
  // number of fully written rows is (_baseIndex - 1) + 1.
  return _baseIndex;

  // Disregarding unsignedness, we could also write:
  //   lastWrittenIndex = produced()
  //     ? _baseIndex
  //     : _baseIndex - 1;
  //   return lastWrittenIndex + 1;
}

void OutputAqlItemRow::advanceRow() {
  TRI_ASSERT(produced());
  TRI_ASSERT(allValuesWritten());
  TRI_ASSERT(_inputRowCopied);
  ++_baseIndex;
  _inputRowCopied = false;
  _numValuesWritten = 0;
}

void OutputAqlItemRow::toVelocyPack(velocypack::Options const* options, VPackBuilder& builder) {
  TRI_ASSERT(produced());
  block().rowToSimpleVPack(_baseIndex, options, builder);
}

SharedAqlItemBlockPtr OutputAqlItemRow::stealBlock() {
  // numRowsWritten() inspects _block, so save this before resetting it!
  auto const numRows = numRowsWritten();
  // Release our hold on the block now
  SharedAqlItemBlockPtr block = std::move(_block);
  if (numRows == 0) {
    // blocks may not be empty
    block = nullptr;
  } else {
    // numRowsWritten() returns the exact number of rows that were fully
    // written and takes into account whether the current row was written.
    block->shrink(numRows);

    if (_doNotCopyInputRow) {
      block->clearRegisters(registersToClear());
    }
  }

  return block;
}

void OutputAqlItemRow::setBaseIndex(std::size_t index) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  _setBaseIndexNotUsed = false;
#endif
  _baseIndex = index;
}

void OutputAqlItemRow::setMaxBaseIndex(std::size_t index) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  _setBaseIndexNotUsed = true;
#endif
  _baseIndex = index;
}

void OutputAqlItemRow::createShadowRow(InputAqlItemRow const& sourceRow) {
  TRI_ASSERT(!_inputRowCopied);
  // A shadow row can only be created on blocks that DO NOT write additional
  // output. This assertion is not a hard requirement and could be softened, but
  // it makes the logic much easier to understand, we have a shadow-row producer
  // that does not produce anything else.
  TRI_ASSERT(numRegistersToWrite() == 0);
  // This is the hard requirement, if we decide to remove the no-write policy.
  // This has te be in place. However no-write implies this.
  TRI_ASSERT(allValuesWritten());
  TRI_ASSERT(sourceRow.isInitialized());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // We can only add shadow rows if source and this are different blocks
  TRI_ASSERT(!sourceRow.internalBlockIs(_block));
#endif
  block().makeShadowRow(_baseIndex);
  doCopyRow(sourceRow, true);
}

void OutputAqlItemRow::increaseShadowRowDepth(ShadowAqlItemRow const& sourceRow) {
  doCopyRow(sourceRow, false);
  block().setShadowRowDepth(_baseIndex,
                            AqlValue{AqlValueHintUInt{sourceRow.getDepth() + 1}});
  // We need to fake produced state
  _numValuesWritten = numRegistersToWrite();
  TRI_ASSERT(produced());
}

void OutputAqlItemRow::decreaseShadowRowDepth(ShadowAqlItemRow const& sourceRow) {
  doCopyRow(sourceRow, false);
  TRI_ASSERT(!sourceRow.isRelevant());
  block().setShadowRowDepth(_baseIndex,
                            AqlValue{AqlValueHintUInt{sourceRow.getDepth() - 1}});
  // We need to fake produced state
  _numValuesWritten = numRegistersToWrite();
  TRI_ASSERT(produced());
}

template <>
void OutputAqlItemRow::memorizeRow<InputAqlItemRow>(InputAqlItemRow const& sourceRow) {
  _lastSourceRow = sourceRow;
  _lastBaseIndex = _baseIndex;
}

template <>
void OutputAqlItemRow::memorizeRow<ShadowAqlItemRow>(ShadowAqlItemRow const& sourceRow) {}

template <>
bool OutputAqlItemRow::testIfWeMustClone<InputAqlItemRow>(InputAqlItemRow const& sourceRow) const {
  return _baseIndex == 0 || _lastSourceRow != sourceRow;
}

template <>
bool OutputAqlItemRow::testIfWeMustClone<ShadowAqlItemRow>(ShadowAqlItemRow const& sourceRow) const {
  return true;
}

template <>
void OutputAqlItemRow::adjustShadowRowDepth<InputAqlItemRow>(InputAqlItemRow const& sourceRow) {
}

template <>
void OutputAqlItemRow::adjustShadowRowDepth<ShadowAqlItemRow>(ShadowAqlItemRow const& sourceRow) {
  block().setShadowRowDepth(_baseIndex, sourceRow.getShadowDepthValue());
}

template <class ItemRowType>
void OutputAqlItemRow::doCopyRow(ItemRowType const& sourceRow, bool ignoreMissing) {
  // Note that _lastSourceRow is invalid right after construction. However, when
  // _baseIndex > 0, then we must have seen one row already.
  TRI_ASSERT(!_doNotCopyInputRow);
  bool mustClone = testIfWeMustClone(sourceRow);

  if (mustClone) {
    for (auto itemId : registersToKeep()) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      if (!_allowSourceRowUninitialized) {
        TRI_ASSERT(sourceRow.isInitialized());
      }
#endif
      if (ignoreMissing && itemId >= sourceRow.getNrRegisters()) {
        continue;
      }
      if (ADB_LIKELY(!_allowSourceRowUninitialized || sourceRow.isInitialized())) {
        auto const& value = sourceRow.getValue(itemId);
        if (!value.isEmpty()) {
          AqlValue clonedValue = value.clone();
          AqlValueGuard guard(clonedValue, true);

          TRI_IF_FAILURE("OutputAqlItemRow::copyRow") {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
          }
          TRI_IF_FAILURE("ExecutionBlock::inheritRegisters") {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
          }

          block().setValue(_baseIndex, itemId, clonedValue);
          guard.steal();
        }
      }
    }
    adjustShadowRowDepth(sourceRow);
  } else {
    TRI_ASSERT(_baseIndex > 0);
    block().copyValuesFromRow(_baseIndex, registersToKeep(), _lastBaseIndex);
  }

  _inputRowCopied = true;
  memorizeRow(sourceRow);
}

template void OutputAqlItemRow::copyRow<InputAqlItemRow>(InputAqlItemRow const& sourceRow,
                                                         bool ignoreMissing);
template void OutputAqlItemRow::copyRow<ShadowAqlItemRow>(ShadowAqlItemRow const& sourceRow,
                                                          bool ignoreMissing);
template void OutputAqlItemRow::cloneValueInto<InputAqlItemRow>(
    RegisterId registerId, const InputAqlItemRow& sourceRow, AqlValue const& value);
template void OutputAqlItemRow::cloneValueInto<ShadowAqlItemRow>(
    RegisterId registerId, const ShadowAqlItemRow& sourceRow, AqlValue const& value);
template void OutputAqlItemRow::moveValueInto<InputAqlItemRow>(RegisterId registerId,
                                                               InputAqlItemRow const& sourceRow,
                                                               AqlValueGuard& guard);
template void OutputAqlItemRow::moveValueInto<ShadowAqlItemRow>(
    RegisterId registerId, ShadowAqlItemRow const& sourceRow, AqlValueGuard& guard);
template void OutputAqlItemRow::doCopyRow<InputAqlItemRow>(InputAqlItemRow const& sourceRow,
                                                           bool ignoreMissing);
template void OutputAqlItemRow::doCopyRow<ShadowAqlItemRow>(ShadowAqlItemRow const& sourceRow,
                                                            bool ignoreMissing);
