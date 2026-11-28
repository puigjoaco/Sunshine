/** @file tests/unit/test_stream_resize.cpp
 * @brief Porta protocol parsing, coalescing and rollback without a display.
 */
#include "src/stream_resize.h"

#include <gtest/gtest.h>

TEST(StreamResizeTest, ParsesExactLittleEndianPayloadAndRejectsTruncation) {
  const std::string bytes {"\x78\x56\x34\x12\xa0\x05\x30\x0c", 8};
  const auto request = stream_resize::parse(bytes);
  ASSERT_TRUE(request);
  EXPECT_EQ(request->sequence, 0x12345678U);
  EXPECT_EQ(request->width, 1440);
  EXPECT_EQ(request->height, 3120);
  for (size_t length = 0; length < bytes.size(); ++length) {
    EXPECT_FALSE(stream_resize::parse(std::string_view(bytes).substr(0, length)));
  }
  EXPECT_FALSE(stream_resize::parse(bytes + "x"));
}

TEST(StreamResizeTest, ValidatesEvenBoundsAndNonzeroSequence) {
  EXPECT_TRUE(stream_resize::valid({1, 256, 4096}));
  EXPECT_TRUE(stream_resize::valid({UINT32_MAX, 4096, 256}));
  for (const auto value : {0, 254, 255, 257, 4095, 4097, 65535}) {
    EXPECT_FALSE(stream_resize::valid({1, uint16_t(value), 1440}));
    EXPECT_FALSE(stream_resize::valid({1, 1440, uint16_t(value)}));
  }
  EXPECT_FALSE(stream_resize::valid({0, 1440, 3120}));
}

TEST(StreamResizeTest, EncodesExactResultIncludingReservedByte) {
  stream_resize::result_t result {{0x12345678, 1440, 3120}, false};
  const auto bytes = result.payload();
  EXPECT_EQ(std::string(bytes.data(), bytes.size()), std::string("\x78\x56\x34\x12\0\0\xa0\x05\x30\x0c", 10));
  result.failed = true;
  EXPECT_EQ(result.payload()[4], 1);
}

TEST(StreamResizeTest, CoalescesStrictlyIncreasingPendingRequests) {
  stream_resize::state_t state(3120, 1440);
  state.enable(true);
  state.offer({1, 1440, 3120});
  state.offer({3, 1920, 1080});
  state.offer({2, 1440, 3120});
  const auto request = state.begin();
  ASSERT_TRUE(request);
  EXPECT_EQ(request->sequence, 3);
  EXPECT_EQ(request->width, 1920);
  EXPECT_FALSE(state.begin());
  EXPECT_FALSE(state.needs_capture());
  state.offer({4, 1440, 3120});
  state.offer({5, 2560, 1440});
  state.captured(*request);
  EXPECT_FALSE(state.take_result());
  state.encoded();
  const auto result = state.take_result();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->failed);
  EXPECT_EQ(result->mode.sequence, 3);
  EXPECT_EQ(state.begin()->sequence, 5);
}

TEST(StreamResizeTest, RejectsUnsupportedAndInvalidRequestsAtCurrentStreamSize) {
  stream_resize::state_t state(3120, 1440);
  state.offer({1, 1440, 3120});
  const auto result = state.take_result();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->failed);
  EXPECT_EQ(result->mode.width, 3120);
  EXPECT_EQ(result->mode.height, 1440);
  state.enable(true);
  state.offer({2, 1441, 3120});
  EXPECT_TRUE(state.take_result()->failed);
  EXPECT_FALSE(state.needs_capture());
  state.offer({2, 1440, 3120});
  EXPECT_FALSE(state.needs_capture());
  state.offer({0, 1440, 3120});
  EXPECT_FALSE(state.take_result());
}

TEST(StreamResizeTest, FailedEncoderRollsBackBeforeReplyingAndPreservesNewestPending) {
  stream_resize::state_t state(3120, 1440);
  state.enable(true);
  state.offer({1, 1440, 3120});
  const auto request = state.begin();
  state.captured(*request);
  EXPECT_EQ(state.dimensions().height, 3120);
  state.offer({2, 1920, 1080});
  state.failed();
  EXPECT_FALSE(state.take_result());
  EXPECT_TRUE(state.needs_capture());
  const auto rollback = state.begin();
  ASSERT_TRUE(rollback);
  EXPECT_EQ(rollback->sequence, 1);
  EXPECT_EQ(rollback->width, 3120);
  EXPECT_EQ(rollback->height, 1440);
  state.captured(*rollback);
  state.encoded();
  const auto result = state.take_result();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->failed);
  EXPECT_EQ(result->mode.sequence, 1);
  EXPECT_EQ(result->mode.width, 3120);
  EXPECT_EQ(state.begin()->sequence, 2);
}

TEST(StreamResizeTest, CaptureFailureAndOwnershipLossDoNotReportAnUnconfirmedSize) {
  stream_resize::state_t state(3120, 1440);
  state.failed();
  state.encoded();
  EXPECT_FALSE(state.take_result());
  state.enable(true);
  state.offer({1, 1440, 3120});
  state.enable(false);
  const auto rejected = state.take_result();
  ASSERT_TRUE(rejected);
  EXPECT_TRUE(rejected->failed);
  EXPECT_EQ(rejected->mode.width, 3120);
  EXPECT_FALSE(state.begin());
  state.enable(true);
  state.offer({2, 1440, 3120});
  ASSERT_TRUE(state.begin());
  state.failed();
  EXPECT_EQ(state.dimensions().width, 3120);
  const auto rollback = state.begin();
  ASSERT_TRUE(rollback);
  state.captured(*rollback);
  state.encoded();
  EXPECT_TRUE(state.take_result()->failed);
}

TEST(StreamResizeTest, RejectsSequenceWrapAndReportsTheLastConfirmedDimensions) {
  stream_resize::state_t state(3120, 1440);
  state.enable(true);
  state.offer({UINT32_MAX, 1440, 3120});
  state.captured(*state.begin());
  state.encoded();
  EXPECT_EQ(state.take_result()->mode.height, 3120);
  state.offer({1, 3120, 1440});
  EXPECT_FALSE(state.needs_capture());
  EXPECT_FALSE(state.take_result());
}

TEST(StreamResizeTest, NewerRejectionIsNotOverwrittenByAnOlderCompletion) {
  stream_resize::state_t state(3120, 1440);
  state.enable(true);
  state.offer({1, 1440, 3120});
  const auto active = state.begin();
  state.offer({2, 257, 3120});
  state.captured(*active);
  state.encoded();
  const auto result = state.take_result();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->mode.sequence, 2);
  EXPECT_TRUE(result->failed);
  EXPECT_EQ(result->mode.height, 3120);
  EXPECT_EQ(state.dimensions().height, 3120);
}
