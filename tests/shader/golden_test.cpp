// nxmac's shader split held to nxnative's: the made-up games of tests/shader/fixtures/cases (every form the split
// reads, files that do not read, several versions and DLC, other engines' files) are split, and each file must have the
// SHA-256 that nxnative's shader_split.py gave for it, and each printed line be one it printed (expected.tsv and
// lines.tsv, written by fixtures/src/make_fixtures.sh).
#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/fs/crypto.h"
#include "nxmac/shader/profiles.h"
#include "nxmac/shader/split.h"

namespace nxmac::shader {
namespace {

using ::testing::HasSubstr;

std::string Fixture(std::string_view file) { return std::string(NXMAC_SHADER_FIXTURES "/") + std::string(file); }

struct Case {
    std::string name, game, versions, dlc, want, why;
};

std::vector<std::vector<std::string>> Rows(const std::string& file) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(Fixture(file));
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        rows.push_back(absl::StrSplit(line, absl::MaxSplits('\t', file == "lines.tsv" ? 1 : 16)));
    }
    return rows;
}

std::vector<Case> Cases() {
    std::vector<Case> cases;
    for (const std::vector<std::string>& r : Rows("cases.tsv")) {
        if (r.size() == 6) cases.push_back({r[0], r[1], r[2], r[3], r[4], r[5]});
    }
    return cases;
}

const ShaderProfiles& FixtureProfiles() {
    static const ShaderProfiles* const profiles = [] {
        absl::StatusOr<ShaderProfiles> read = ShaderProfiles::ReadFile(Fixture("profiles_fixture.tsv"));
        EXPECT_TRUE(read.ok()) << read.status();
        return new ShaderProfiles(read.ok() ? *std::move(read) : ShaderProfiles());
    }();
    return *profiles;
}

std::string Sha256Hex(std::string_view bytes) {
    const fs::Sha256Hash hash =
        fs::Sha256(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    return absl::BytesToHexString(std::string_view(reinterpret_cast<const char*>(hash.data()), hash.size()));
}

class GoldenTest : public ::testing::TestWithParam<Case> {};

TEST_P(GoldenTest, SplitsAsNxnativeDoes) {
    const Case& c = GetParam();
    const std::string dir = Fixture(absl::StrCat("cases/", c.name, "/"));
    absl::StatusOr<const ShaderProfile*> profile = FixtureProfiles().Find(c.game);
    ASSERT_TRUE(profile.ok()) << profile.status();
    SplitOptions options;
    options.profile = *profile;
    for (const std::string_view v : absl::StrSplit(c.versions, ';')) {
        const std::vector<std::string> parts = absl::StrSplit(v, '|');
        ASSERT_EQ(parts.size(), 2u);
        options.versions.push_back({dir + parts[0], dir + parts[1]});
    }
    if (c.dlc != "-") {
        for (const std::string_view d : absl::StrSplit(c.dlc, ';')) options.dlc.push_back(dir + std::string(d));
    }
    options.jobs = 3;
    std::vector<std::string> printed;
    options.print = [&](const std::string& line) {
        if (!absl::StartsWith(line, "progress: ")) printed.push_back(line);
    };
    const absl::StatusOr<SplitResult> result = Split(options);
    if (c.want == "stop") {
        ASSERT_FALSE(result.ok());
        EXPECT_THAT(std::string(result.status().message()), HasSubstr(c.why));
        return;
    }
    ASSERT_TRUE(result.ok()) << result.status();
    std::map<std::string, std::string> want;
    for (const std::vector<std::string>& r : Rows("expected.tsv")) {
        if (r.size() == 3 && r[0] == c.name) want[r[1]] = r[2];
    }
    std::map<std::string, std::string> got = {{"shader_map.tsv", Sha256Hex(result->shader_map)},
                                              {"shader_facts.tsv", Sha256Hex(result->shader_facts)},
                                              {"shader_pairs.tsv", Sha256Hex(result->shader_pairs)},
                                              {"pso_pairs.tsv", Sha256Hex(result->pso_pairs)}};
    for (const auto& [name, bytes] : result->package) got[absl::StrCat("pkg/", name)] = Sha256Hex(bytes);
    EXPECT_EQ(got, want);
    std::vector<std::string> lines;
    for (const std::vector<std::string>& r : Rows("lines.tsv")) {
        if (r.size() == 2 && r[0] == c.name) lines.push_back(r[1]);
    }
    if (c.want == "own-lines") {
        // another engine's files: left out with nxmac's own red line, and counted as unreadable ones are
        EXPECT_EQ(result->unreadable, 2u);
        EXPECT_EQ(std::count_if(printed.begin(), printed.end(),
                                [](const std::string& l) {
                                    return absl::StrContains(
                                        l, "its reader is not ported to nxmac yet (its shaders are left out)");
                                }),
                  2);
        EXPECT_EQ(printed.back(), lines.back());  // the summary
        return;
    }
    // the lines shader_split.py prints, in the order the files finish (it printed them one file at a time)
    std::sort(printed.begin(), printed.end());
    std::sort(lines.begin(), lines.end());
    EXPECT_EQ(printed, lines);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, GoldenTest, ::testing::ValuesIn(Cases()),
                         [](const ::testing::TestParamInfo<Case>& info) { return info.param.name; });

TEST(GoldenTest, EveryCaseIsThere) { EXPECT_EQ(Cases().size(), 12u); }

TEST(GoldenTest, WritesThePackageAndTables) {
    absl::StatusOr<const ShaderProfile*> profile = FixtureProfiles().Find("01007EF00011E000");
    ASSERT_TRUE(profile.ok());
    SplitOptions options;
    options.profile = *profile;
    options.versions.push_back({Fixture("cases/keyskip/rom"), Fixture("cases/keyskip/main")});
    absl::StatusOr<SplitResult> result = Split(options);
    ASSERT_TRUE(result.ok()) << result.status();
    const std::string out = absl::StrCat(::testing::TempDir(), "shader_golden_write");
    ASSERT_TRUE(WriteSplit(*result, out).ok());
    std::ifstream map(out + "/shader_map.tsv", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(map)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text, result->shader_map);
    for (const auto& [name, bytes] : result->package) {
        std::ifstream in(absl::StrCat(out, "/pkg/", name), std::ios::binary);
        const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(got, bytes) << name;
    }
}

}  // namespace
}  // namespace nxmac::shader
