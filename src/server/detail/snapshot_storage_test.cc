// Copyright 2026, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.

#include "server/detail/snapshot_storage.h"

#include "base/gtest.h"

namespace dfly::detail {

TEST(AzurePathTest, ParsesAccountQualifiedUri) {
  auto path = ParseAzurePath("https://testaccount.blob.core.windows.net/snapshots/daily/dump.rdb");

  ASSERT_TRUE(path);
  EXPECT_EQ(path->account, "testaccount");
  EXPECT_EQ(path->container, "snapshots");
  EXPECT_EQ(path->key, "daily/dump.rdb");
  EXPECT_EQ(BuildAzurePath(*path),
            "https://testaccount.blob.core.windows.net/snapshots/daily/dump.rdb");
}

TEST(AzurePathTest, AllowsContainerRoot) {
  auto path = ParseAzurePath("https://testaccount.blob.core.windows.net/snapshots");

  ASSERT_TRUE(path);
  EXPECT_EQ(path->key, "");
  EXPECT_EQ(BuildAzurePath(*path), "https://testaccount.blob.core.windows.net/snapshots");
}

TEST(AzurePathTest, DecodesAndEncodesEscapedPathComponents) {
  auto path = ParseAzurePath("https://testaccount.blob.core.windows.net/snapshots/dump%20file.rdb");

  ASSERT_TRUE(path);
  EXPECT_EQ(path->container, "snapshots");
  EXPECT_EQ(path->key, "dump file.rdb");
  EXPECT_EQ(BuildAzurePath(*path),
            "https://testaccount.blob.core.windows.net/snapshots/dump%20file.rdb");
}

TEST(AzurePathTest, RejectsInvalidUris) {
  for (std::string_view path :
       {"az://testaccount.blob.core.windows.net/snapshots/dump.rdb", "https://snapshots/dump.rdb",
        "https://.blob.core.windows.net/snapshots/dump.rdb",
        "https://testaccount.blob.core.windows.net",
        "https://test.account.blob.core.windows.net/snapshots/dump.rdb",
        "https://testaccount.blob.core.windows.net/container--name/dump.rdb",
        "https://testaccount.blob.core.windows.net/container%2Fname/dump.rdb",
        "https://testaccount.blob.core.windows.net/snapshots%2",
        "https://testaccount.blob.core.windows.net/snapshots/dump.rdb?sig"}) {
    EXPECT_FALSE(ParseAzurePath(path));
  }
}

TEST(AzurePathTest, ClassifiesOnlyAzureHttpsUris) {
  EXPECT_TRUE(IsAzurePath("https://testaccount.blob.core.windows.net/snapshots/dump.rdb"));
  EXPECT_FALSE(IsAzurePath("https://example.com/snapshots/dump.rdb"));
  EXPECT_FALSE(IsAzurePath("az://testaccount.blob.core.windows.net/snapshots/dump.rdb"));
}

}  // namespace dfly::detail
