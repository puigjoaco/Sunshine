/**
 * @file tests/unit/platform/linux/test_kwin_text.cpp
 * @brief Validate Unicode decoding and ensure text never gains an implicit Return.
 */
#include "src/platform/linux/kwin_text.h"

#include <gtest/gtest.h>

TEST(KwinTextTest, EncodesAccentsPunctuationEuroAndEmojiWithoutAnImplicitReturn) {
  const auto result = kwin::text_keysyms("año ¿qué? ¡sí! €😀");
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, (std::vector<std::uint32_t> {'a', 0xF1, 'o', ' ', 0xBF, 'q', 'u', 0xE9, '?', ' ', 0xA1, 's', 0xED, '!', ' ', 0x010020AC, 0x0101F600}));
}

TEST(KwinTextTest, EmitsReturnAndTabOnlyWhenExplicitlyPresentInText) {
  const auto result = kwin::text_keysyms("a\n\tb");
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, (std::vector<std::uint32_t> {'a', 0xFF0D, 0xFF09, 'b'}));
  EXPECT_TRUE(kwin::text_keysyms("")->empty());
}

TEST(KwinTextTest, RejectsMalformedUtf8SurrogatesAndControlCodesBeforeAnyInjection) {
  for (const auto text : {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xE2\x82", "\xC2x", "\x80", "\x7F", "\x01", "\xE0\x80\x80"}) {
    EXPECT_FALSE(kwin::text_keysyms(text)) << text;
  }
}

#ifdef SUNSHINE_BUILD_KWIN
#include <linux/input-event-codes.h>

TEST(KwinTextTest, SharedKeyboardMapsPositionsAndBothModifierSides) {
  EXPECT_EQ(kwin::evdev_key(0x41), KEY_A);
  EXPECT_EQ(kwin::evdev_key(0x4F), KEY_O);
  EXPECT_EQ(kwin::evdev_key(0x0D), KEY_ENTER);
  EXPECT_EQ(kwin::evdev_key(0xA1), KEY_RIGHTSHIFT);
  EXPECT_EQ(kwin::evdev_key(0xA3), KEY_RIGHTCTRL);
  EXPECT_EQ(kwin::evdev_key(0xA5), KEY_RIGHTALT);
  EXPECT_EQ(kwin::evdev_key(0x5B), KEY_LEFTMETA);
  EXPECT_EQ(kwin::evdev_key(0x2C), KEY_SYSRQ);
  EXPECT_EQ(kwin::evdev_key(0x87), KEY_F24);
  EXPECT_EQ(kwin::evdev_key(0), -1);
}
#endif
