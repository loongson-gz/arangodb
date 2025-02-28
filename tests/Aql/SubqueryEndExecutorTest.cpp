////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include "AqlItemBlockHelper.h"
#include "RowFetcherHelper.h"
#include "gtest/gtest.h"

#include "Mocks/Death_Test.h"

#include "Aql/OutputAqlItemRow.h"
#include "Aql/RegisterPlan.h"
#include "Aql/SubqueryEndExecutor.h"

#include "Logger/LogMacros.h"

#include "Basics/VelocyPackHelper.h"

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::tests;
using namespace arangodb::tests::aql;
using namespace arangodb::basics;

using RegisterSet = std::unordered_set<RegisterId>;

class SubqueryEndExecutorTest : public ::testing::Test {
 public:
  SubqueryEndExecutorTest()
      : _infos(std::make_shared<RegisterSet>(std::initializer_list<RegisterId>({0})),
               std::make_shared<RegisterSet>(std::initializer_list<RegisterId>({0})),
               1, 1, {}, {}, nullptr, RegisterId{0}, RegisterId{0}) {}

 protected:
  ResourceMonitor monitor;
  AqlItemBlockManager itemBlockManager{&monitor, SerializationFormat::SHADOWROWS};
  SubqueryEndExecutorInfos _infos;

  void ExpectedValues(OutputAqlItemRow& itemRow,
                      std::vector<std::vector<std::string>> const& expectedStrings,
                      std::unordered_map<size_t, uint64_t> const& shadowRows) const {
    auto block = itemRow.stealBlock();

    ASSERT_EQ(expectedStrings.size(), block->size());

    for (size_t rowIdx = 0; rowIdx < block->size(); rowIdx++) {
      if (block->isShadowRow(rowIdx)) {
        ShadowAqlItemRow shadow{block, rowIdx};

        auto depth = shadowRows.find(rowIdx);
        if (depth != shadowRows.end()) {
          EXPECT_EQ(depth->second, shadow.getDepth());
        } else {
          FAIL() << "did not expect row " << rowIdx << " to be a shadow row";
        }
      } else {
        EXPECT_EQ(shadowRows.find(rowIdx), shadowRows.end())
            << "expected row " << rowIdx << " to be a shadow row";

        InputAqlItemRow input{block, rowIdx};
        for (unsigned int colIdx = 0; colIdx < block->getNrRegs(); colIdx++) {
          auto expected = VPackParser::fromJson(expectedStrings.at(rowIdx).at(colIdx));
          auto value = input.getValue(RegisterId{colIdx}).slice();
          EXPECT_TRUE(VelocyPackHelper::equal(value, expected->slice(), false))
              << value.toJson() << " != " << expected->toJson();
        }
      }
    }
  }
};

TEST_F(SubqueryEndExecutorTest, check_properties) {
  EXPECT_TRUE(SubqueryEndExecutor::Properties::preservesOrder)
      << "The block has no effect on ordering of elements, it adds additional "
         "rows only.";
  EXPECT_EQ(SubqueryEndExecutor::Properties::allowsBlockPassthrough, ::arangodb::aql::BlockPassthrough::Disable)
      << "The block cannot be passThrough, as it increases the number of rows.";
  EXPECT_TRUE(SubqueryEndExecutor::Properties::inputSizeRestrictsOutputSize)
      << "The block produces one output row per input row plus potentially a "
         "shadow rows which is bounded by the structure of the query";
};

// If the input to a spliced subquery is empty, there should be no output
TEST_F(SubqueryEndExecutorTest, empty_input_expects_no_shadow_rows) {
  SharedAqlItemBlockPtr outputBlock;
  SharedAqlItemBlockPtr inputBlock = buildBlock<1>(itemBlockManager, {{{}}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, 1, false, inputBlock);
  SubqueryEndExecutor testee(fetcher, _infos);

  // I don't seem to be able to make an empty inputBlock above,
  // so we just fetch the one row that's in the block.
  fetcher.fetchRow();

  ExecutionState state{ExecutionState::HASMORE};

  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 1));
  OutputAqlItemRow output{std::move(outputBlock), _infos.getOutputRegisters(),
                          _infos.registersToKeep(), _infos.registersToClear()};

  std::tie(state, std::ignore) = testee.produceRows(output);
  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(output.numRowsWritten(), 0);
}

TEST_F(SubqueryEndExecutorTest, single_input_expects_shadow_rows) {
  SharedAqlItemBlockPtr outputBlock;
  SharedAqlItemBlockPtr inputBlock =
      buildBlock<1>(itemBlockManager, {{{1}}, {{1}}}, {{1, 0}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, inputBlock->size(), false, inputBlock);

  SubqueryEndExecutor testee(fetcher, _infos);

  ExecutionState state{ExecutionState::HASMORE};
  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 1));
  OutputAqlItemRow output{std::move(outputBlock), _infos.getOutputRegisters(),
                          _infos.registersToKeep(), _infos.registersToClear()};
  std::tie(state, std::ignore) = testee.produceRows(output);
  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(output.numRowsWritten(), 1);

  ExpectedValues(output, {{"[1]"}}, {});
}

TEST_F(SubqueryEndExecutorTest, two_inputs_one_shadowrow) {
  SharedAqlItemBlockPtr outputBlock;
  SharedAqlItemBlockPtr inputBlock =
      buildBlock<1>(itemBlockManager, {{{42}}, {{34}}, {{1}}}, {{2, 0}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, inputBlock->size(), false, inputBlock);

  SubqueryEndExecutor testee(fetcher, _infos);

  ExecutionState state{ExecutionState::HASMORE};

  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 1));
  OutputAqlItemRow output{std::move(outputBlock), _infos.getOutputRegisters(),
                          _infos.registersToKeep(), _infos.registersToClear()};
  std::tie(state, std::ignore) = testee.produceRows(output);
  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(output.numRowsWritten(), 1);

  ExpectedValues(output, {{"[42,34]"}}, {});
}

TEST_F(SubqueryEndExecutorTest, two_inputs_two_shadowrows) {
  SharedAqlItemBlockPtr outputBlock;

  SharedAqlItemBlockPtr inputBlock =
      buildBlock<1>(itemBlockManager, {{{42}}, {{1}}, {{34}}, {{1}}}, {{1, 0}, {3, 0}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, inputBlock->size(), false, inputBlock);

  SubqueryEndExecutor testee(fetcher, _infos);

  ExecutionState state{ExecutionState::HASMORE};

  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 1));
  OutputAqlItemRow output{std::move(outputBlock), _infos.getOutputRegisters(),
                          _infos.registersToKeep(), _infos.registersToClear()};
  std::tie(state, std::ignore) = testee.produceRows(output);
  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(output.numRowsWritten(), 2);
  ExpectedValues(output, {{"[42]"}, {"[34]"}}, {});
}

TEST_F(SubqueryEndExecutorTest, two_input_one_shadowrow_two_irrelevant) {
  SharedAqlItemBlockPtr outputBlock;
  SharedAqlItemBlockPtr inputBlock =
      buildBlock<1>(itemBlockManager, {{{42}}, {{42}}, {{42}}, {{42}}, {{42}}},
                    {{2, 0}, {3, 1}, {4, 2}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, inputBlock->size(), false, inputBlock);

  SubqueryEndExecutor testee(fetcher, _infos);

  ExecutionState state{ExecutionState::HASMORE};

  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 1));
  OutputAqlItemRow output{std::move(outputBlock), _infos.getOutputRegisters(),
                          _infos.registersToKeep(), _infos.registersToClear()};

  std::tie(state, std::ignore) = testee.produceRows(output);
  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(output.numRowsWritten(), 3);
  ExpectedValues(output, {{{"[42, 42]"}}, {{""}}, {{""}}}, {{1, 0}, {2, 1}});
}

TEST_F(SubqueryEndExecutorTest, consume_output_of_subquery_end_executor) {
  ExecutionState state{ExecutionState::HASMORE};

  SharedAqlItemBlockPtr outputBlock;
  SharedAqlItemBlockPtr inputBlock =
      buildBlock<1>(itemBlockManager, {{{42}}, {{42}}, {{42}}, {{42}}, {{42}}},
                    {{2, 0}, {3, 1}, {4, 2}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, inputBlock->size(), false, inputBlock);

  SubqueryEndExecutor testee(fetcher, _infos);

  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 1));
  OutputAqlItemRow output{std::move(outputBlock), _infos.getOutputRegisters(),
                          _infos.registersToKeep(), _infos.registersToClear()};
  std::tie(state, std::ignore) = testee.produceRows(output);
  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(output.numRowsWritten(), 3);

  //  ExpectedValues(output, { "[42, 42]", "", "" });

  outputBlock = output.stealBlock();
  inputBlock.swap(outputBlock);
  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher2(
      itemBlockManager, inputBlock->size(), false, inputBlock);
  SubqueryEndExecutor testee2(fetcher2, _infos);
  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 1));
  OutputAqlItemRow output2{std::move(outputBlock), _infos.getOutputRegisters(),
                           _infos.registersToKeep(), _infos.registersToClear()};
  std::tie(state, std::ignore) = testee2.produceRows(output2);
  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(output2.numRowsWritten(), 2);

  ExpectedValues(output2, {{"[ [42, 42] ]"}, {""}}, {{1, 0}});
}

TEST_F(SubqueryEndExecutorTest, write_to_register_outside) {
  auto infos = SubqueryEndExecutorInfos(
      std::make_shared<RegisterSet>(std::initializer_list<RegisterId>{0}),
      std::make_shared<RegisterSet>(std::initializer_list<RegisterId>{1}), 1, 2,
      {}, RegisterSet{0}, nullptr, RegisterId{0}, RegisterId{1});

  ExecutionState state{ExecutionState::HASMORE};

  SharedAqlItemBlockPtr outputBlock;
  SharedAqlItemBlockPtr inputBlock =
      buildBlock<1>(itemBlockManager, {{{42}}, {{23}}}, {{1, 0}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, inputBlock->size(), false, inputBlock);

  SubqueryEndExecutor testee(fetcher, infos);

  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 2));
  OutputAqlItemRow output{std::move(outputBlock), infos.getOutputRegisters(),
                          infos.registersToKeep(), infos.registersToClear()};
  std::tie(state, std::ignore) = testee.produceRows(output);
  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(output.numRowsWritten(), 1);

  ExpectedValues(output, {{"23", "[42]"}}, {{1, 0}});
}

TEST_F(SubqueryEndExecutorTest, no_input_register) {
  auto infos = SubqueryEndExecutorInfos(
      std::make_shared<RegisterSet>(std::initializer_list<RegisterId>{0}),
      std::make_shared<RegisterSet>(std::initializer_list<RegisterId>{1}), 1, 2, {},
      RegisterSet{0}, nullptr, RegisterId{RegisterPlan::MaxRegisterId}, RegisterId{1});

  ExecutionState state{ExecutionState::HASMORE};

  SharedAqlItemBlockPtr outputBlock;
  SharedAqlItemBlockPtr inputBlock =
      buildBlock<1>(itemBlockManager, {{{42}}, {{23}}}, {{1, 0}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, inputBlock->size(), false, inputBlock);

  SubqueryEndExecutor testee(fetcher, infos);

  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 2));
  OutputAqlItemRow output{std::move(outputBlock), infos.getOutputRegisters(),
                          infos.registersToKeep(), infos.registersToClear()};
  std::tie(state, std::ignore) = testee.produceRows(output);
  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(output.numRowsWritten(), 1);

  ExpectedValues(output, {{"23", "[]"}}, {{1, 0}});
}

// TODO: This is a "death test" with malformed shadow row layout (an irrelevant shadow row before any other row)
// See https://github.com/google/googletest/blob/master/googletest/docs/advanced.md#death-tests-and-threads

using SubqueryEndExecutorTest_DeathTest = SubqueryEndExecutorTest;

TEST_F(SubqueryEndExecutorTest_DeathTest, no_shadow_row) {
  SharedAqlItemBlockPtr outputBlock;
  SharedAqlItemBlockPtr inputBlock = buildBlock<1>(itemBlockManager, {{1}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, inputBlock->size(), false, inputBlock);

  SubqueryEndExecutor testee(fetcher, _infos);

  ExecutionState state{ExecutionState::HASMORE};

  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 1));
  OutputAqlItemRow output{std::move(outputBlock), _infos.getOutputRegisters(),
                          _infos.registersToKeep(), _infos.registersToClear()};
  EXPECT_DEATH_CORE_FREE(std::tie(state, std::ignore) = testee.produceRows(output),
                         ".*");
}

TEST_F(SubqueryEndExecutorTest_DeathTest, misplaced_irrelevant_shadowrow) {
  SharedAqlItemBlockPtr outputBlock;
  SharedAqlItemBlockPtr inputBlock =
      buildBlock<1>(itemBlockManager, {{42}, {42}, {42}}, {{1, 1}, {2, 1}});

  SingleRowFetcherHelper<::arangodb::aql::BlockPassthrough::Disable> fetcher(
      itemBlockManager, inputBlock->size(), false, inputBlock);

  SubqueryEndExecutor testee(fetcher, _infos);

  ExecutionState state{ExecutionState::HASMORE};

  outputBlock.reset(new AqlItemBlock(itemBlockManager, inputBlock->size(), 1));
  OutputAqlItemRow output{std::move(outputBlock), _infos.getOutputRegisters(),
                          _infos.registersToKeep(), _infos.registersToClear()};
  EXPECT_DEATH_CORE_FREE(std::tie(state, std::ignore) = testee.produceRows(output),
                         ".*");
}
