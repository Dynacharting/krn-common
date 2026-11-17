// ENC-1646 (SPEC 2026-11-17-order-flow-pipelines D4): docs/atomic-keys.md's
// flow.* table lists exactly the keys the component and its operators
// declare, each marked the way it is published (pushed vs AtomicAccessor-only).
// An operator that adds a key adds a row, or this fails.
#include "krn/flow/FlowComponent.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace krn;

namespace {

// Rows of the form  | `flow.x` | pushed | …   or   | `flow.x` | AtomicAccessor-only | …
// inside the "## `flow.*` vocabulary" section.
std::map<std::string, std::string> documentedKeys(std::string* why) {
  const std::string path = std::string(KRN_COMMON_SOURCE_DIR) + "/docs/atomic-keys.md";
  std::ifstream in(path);
  if (!in) { *why = "cannot open " + path; return {}; }
  std::map<std::string, std::string> out;
  std::string line;
  bool inSection = false;
  const std::regex row(R"(^\|\s*`(flow\.[A-Za-z0-9_.]+)`\s*\|\s*([A-Za-z-]+)\s*\|)");
  while (std::getline(in, line)) {
    if (line.rfind("## ", 0) == 0) { inSection = line.find("`flow.*` vocabulary") != std::string::npos; continue; }
    if (!inSection) continue;
    std::smatch m;
    if (std::regex_search(line, m, row)) {
      if (out.count(m[1])) *why += "duplicate row " + m[1].str() + "; ";
      out[m[1]] = m[2];
    }
  }
  return out;
}

} // namespace

TEST(FlowVocabularyTest, DocsListEveryKeyWithItsPublication) {
  std::string why;
  const auto docs = documentedKeys(&why);
  ASSERT_FALSE(docs.empty()) << "no flow.* rows found: " << why;
  EXPECT_TRUE(why.empty()) << why;

  flow::FlowComponent c(flow::FlowComponent::Env{});
  std::map<std::string, std::string> code;
  for (const auto& k : c.vocabulary()) {
    EXPECT_EQ(code.count(k.name), 0u) << "declared twice: " << k.name;
    code[k.name] = k.publication == flow::Publication::Pushed ? "pushed" : "AtomicAccessor-only";
  }
  EXPECT_EQ(docs, code);
  // D4: the vocabulary counts against AtomicStore's per-symbol field cap.
  EXPECT_LE(code.size(), 60u);
}
