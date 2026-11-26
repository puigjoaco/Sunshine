/** @file tests/unit/test_camera.cpp
 * @brief Bounds, reassembly, sequence and decoder recovery checks for camera v1.
 */
#include <gtest/gtest.h>
#include <src/camera.h>

namespace {
  /** @brief Build a canonical two-chunk frame. */
  camera::chunk_t chunk(uint32_t seq, int index = 0, bool key = true) {
    return {key, false, 0, seq, uint16_t(index), 2, 1000, std::string(index ? 5 : camera::chunk_bytes, char('a' + index))};
  }
}  // namespace

TEST(Camera, WireBoundsAndLittleEndianMetadata) {
  EXPECT_FALSE(camera::parse(""));
  std::string p(24, 0);
  p[0] = 1;
  p[1] = 2;
  ASSERT_TRUE(camera::parse(p));
  p[1] = 3;
  EXPECT_FALSE(camera::parse(p));
  p[1] = 0;
  EXPECT_FALSE(camera::parse(p));
  p[14] = 1;
  p += 'x';
  p[2] = 90;
  p[8] = char(0xfe);
  p[11] = char(0x80);
  p[23] = 1;
  auto c = camera::parse(p);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->sequence, 0x800000feu);
  EXPECT_EQ(c->timestamp, 1ull << 56);
  EXPECT_EQ(c->rotation, 90);
  p[2] = 91;
  EXPECT_FALSE(camera::parse(p));
  p[2] = 90;
  p[4] = 1;
  EXPECT_FALSE(camera::parse(p));
  p[4] = 0;
  p[12] = 1;
  EXPECT_FALSE(camera::parse(p));
  p[12] = 0;
  p[15] = 3;
  EXPECT_FALSE(camera::parse(p));
  p[15] = 0;
  p[14] = 2;
  EXPECT_FALSE(camera::parse(p));
  p.resize(24 + 768, 'x');
  EXPECT_TRUE(camera::parse(p));
  p += 'x';
  EXPECT_FALSE(camera::parse(p));
}

TEST(Camera, ReordersChunksAndRejectsDuplicatesAndReplays) {
  camera::assembler_t q;
  EXPECT_FALSE(q.push(chunk(4, 1)));
  EXPECT_FALSE(q.push(chunk(4, 1)));
  auto f = q.push(chunk(4));
  ASSERT_TRUE(f);
  EXPECT_EQ(f->data, std::string(768, 'a') + std::string(5, 'b'));
  EXPECT_FALSE(q.push(chunk(4)));
  EXPECT_FALSE(q.push(chunk(3)));
  EXPECT_FALSE(q.push(chunk(5, 0, false)));
  EXPECT_TRUE(q.push(chunk(5, 1, false)));
}

TEST(Camera, LostChunkOrWholeFrameFreezesUntilCompleteIdr) {
  camera::assembler_t q;
  q.push(chunk(0));
  ASSERT_TRUE(q.push(chunk(0, 1)));
  q.push(chunk(1));  // Last chunk is lost.
  EXPECT_FALSE(q.push(chunk(2, 0, false)));
  EXPECT_FALSE(q.push(chunk(2, 1, false)));
  EXPECT_FALSE(q.push(chunk(3)));
  EXPECT_TRUE(q.push(chunk(3, 1)));
  EXPECT_FALSE(q.push(chunk(5, 0, false)));
  EXPECT_FALSE(q.push(chunk(5, 1, false)));
  q.push(chunk(6));
  EXPECT_TRUE(q.push(chunk(6, 1)));
}

TEST(Camera, StopBeforeFirstFrameRetainsReplayBoundaryAcrossWrap) {
  camera::assembler_t q;
  auto stop = q.push({false, true, 0, 0xffffffffu, 0, 0, 0, {}});
  ASSERT_TRUE(stop);
  EXPECT_TRUE(stop->stop);
  EXPECT_FALSE(q.push(chunk(0xfffffffeu)));
  EXPECT_FALSE(q.push(chunk(0xffffffffu)));
  EXPECT_FALSE(q.push(chunk(0, 0, false)));
  EXPECT_FALSE(q.push(chunk(0, 1, false)));
  q.push(chunk(1));
  EXPECT_TRUE(q.push(chunk(1, 1)));
  EXPECT_FALSE(q.push({false, true, 0, 0xffffffffu, 0, 0, 0, {}}));
}

TEST(Camera, InconsistentMetadataCannotProduceCorruptAccessUnit) {
  camera::assembler_t q;
  q.push(chunk(0));
  auto c = chunk(0, 1);
  c.rotation = 90;
  EXPECT_FALSE(q.push(c));
  EXPECT_FALSE(q.push(chunk(0, 1)));
  EXPECT_FALSE(q.push(chunk(1, 0, false)));
  EXPECT_FALSE(q.push(chunk(1, 1, false)));
  q.push(chunk(2));
  EXPECT_TRUE(q.push(chunk(2, 1)));
}

TEST(Camera, ExplicitQueueInvalidationRequiresFreshIdr) {
  camera::assembler_t q;
  q.push(chunk(0));
  ASSERT_TRUE(q.push(chunk(0, 1)));
  q.invalidate();
  EXPECT_FALSE(q.push(chunk(1, 0, false)));
  EXPECT_FALSE(q.push(chunk(1, 1, false)));
  q.push(chunk(2));
  EXPECT_TRUE(q.push(chunk(2, 1)));
}
