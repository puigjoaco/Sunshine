/** @file tests/unit/platform/linux/test_kwin_permission.cpp
 * @brief Ensure an old matching desktop file gains Unicode input permission idempotently.
 */
#include "src/platform/linux/kwin_permission.h"

#include <gtest/gtest.h>

TEST(KwinPermissionTest, UpgradesOldEntryWithoutLosingExecOrUnrelatedInterfaces) {
  const std::string old = "[Desktop Entry]\nExec=/test/sunshine\nX-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,other\nNoDisplay=true\n";
  EXPECT_FALSE(kwin::complete_permission_interfaces(old));
  auto upgraded = kwin::upgrade_permission_interfaces(old);
  EXPECT_NE(upgraded.find("Exec=/test/sunshine\n"), std::string::npos);
  EXPECT_NE(upgraded.find("zkde_screencast_unstable_v1,other,org_kde_kwin_fake_input"), std::string::npos);
  EXPECT_TRUE(kwin::complete_permission_interfaces(upgraded));
  EXPECT_EQ(kwin::upgrade_permission_interfaces(upgraded), upgraded);
}

TEST(KwinPermissionTest, AddsMissingListAndRequiresExactTokens) {
  auto result = kwin::upgrade_permission_interfaces("[Desktop Entry]\nExec=/test/sunshine\n");
  EXPECT_TRUE(kwin::complete_permission_interfaces(result));
  EXPECT_FALSE(kwin::complete_permission_interfaces("X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input_old\n"));
}
