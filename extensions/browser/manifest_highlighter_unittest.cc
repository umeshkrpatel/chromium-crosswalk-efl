// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/manifest_highlighter.h"

#include <string>

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace extensions {

namespace {

const char kManifest[] =
"{\n"
"  \"name\": \"Content Scripts\",\n"
"  \"version\": \"2.0\",\n"
"  // this is a comment with the word permissions.\n"
"  /* This is a multine\n"
"     comment with the word permissions\n"
"     that shouldn't be highlighted */\n"
"  \"permissions\": [\n"
"    /* This is a tricky comment because it has brackets }]*/\n"
"    \"tabs\"\n"
"  ],\n"
"  \"content_scripts\": [\n"
"    {\n"
"      \"matches\": [\"*://aaronboodman.com/*\", \"*://rdcronin.com/*\"],\n"
"      \"js\": [\"myscript.js\"]\n"
"    }\n"
"  ],\n"
"  \"test_key\": {\n"
"    \"escaped_quoted\\\"\",\n"
"    \"/*foo*/\"\n"
"  },\n"
"  \"manifest_version\": 2,\n"
"  \"international_key\": \"還是不要\"\n"
"}";

}  // namespace

TEST(ManifestHighlighterUnitTest, ManifestHighlighterUnitTest) {
  // Get a full key.
  const char kPermissionsFeature[] =
      "\"permissions\": [\n"
      "    /* This is a tricky comment because it has brackets }]*/\n"
      "    \"tabs\"\n"
      "  ]";
  ManifestHighlighter permissions(kManifest, "permissions", EmptyString());
  EXPECT_EQ(kPermissionsFeature, permissions.GetFeature());

  // Get a specific portion of a key.
  const char kTabsFeature[] = "\"tabs\"";
  ManifestHighlighter tabs(kManifest, "permissions", "tabs");
  EXPECT_EQ(kTabsFeature, tabs.GetFeature());

  // Get a single-character, non-quoted entity of a key.
  const char kManifestVersionFeature[] = "2";
  ManifestHighlighter version(kManifest, "manifest_version", "2");
  EXPECT_EQ(kManifestVersionFeature, version.GetFeature());

  // Get a compound portion of a key, including quoted '//' (which shouldn't be
  // mistaken for comments).
  const char kMatchesFeature[] =
      "\"matches\": [\"*://aaronboodman.com/*\", \"*://rdcronin.com/*\"]";
  ManifestHighlighter matches(kManifest, "content_scripts", "matches");
  EXPECT_EQ(kMatchesFeature, matches.GetFeature());

  // If a feature isn't present, we should get an empty string.
  ManifestHighlighter not_present(kManifest, "a_fake_feature", EmptyString());
  EXPECT_EQ(EmptyString(), not_present.GetFeature());

  // If we request a specific portion of a key which is not found, we should
  // get an empty string.
  ManifestHighlighter specific_portion_not_present(
      kManifest, "permissions", "a_fake_feature");
  EXPECT_EQ(EmptyString(), specific_portion_not_present.GetFeature());

  const char kEscapedQuotedFeature[] = "\"escaped_quoted\\\"\"";
  ManifestHighlighter escaped_quoted(
      kManifest, "test_key", "escaped_quoted\\\"");
  EXPECT_EQ(kEscapedQuotedFeature, escaped_quoted.GetFeature());

  const char kFeatureWithComment[] = "\"/*foo*/\"";
  ManifestHighlighter feature_with_comment(kManifest, "test_key", "/*foo*/");
  EXPECT_EQ(kFeatureWithComment, feature_with_comment.GetFeature());

  // Check with non-ascii characters.
  const char kInternationalFeature[] = "\"international_key\": \"還是不要\"";
  ManifestHighlighter international_feature(
      kManifest, "international_key", EmptyString());
  EXPECT_EQ(kInternationalFeature, international_feature.GetFeature());
}

}  // namespace extensions
