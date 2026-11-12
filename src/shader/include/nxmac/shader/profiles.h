// What the shader split takes from a game profile. nxnative keeps it in dm/games/<id>/game.tsv (shader_groups,
// shader_note, shader_key_skip, modules) and dm/games/<id>/shader_facts_manual.tsv; tools/export_shader_profiles.py
// writes it out as the text file nxmac carries (src/shader/profiles.tsv, built into the library).
//
// One row per line, fields separated by tabs, the first naming the row:
//   source <text>                                   what the table was made from (nxnative's commit)
//   profile <title ID>                              a profile; the rows below name it again
//   groups <title ID> <part>=<group>...             shader_groups: a source whose label holds <part> goes to the
//                                                   package <stage>_<group>.bin; the first that matches wins
//   note <title ID> <text>                          shader_note: the shader map's header names the shaders with it
//   key_skip <title ID> <offset> <length>           shader_key_skip: control bytes the keys hash as zeros
//   modules <title ID> <module>...                  the profile's modules (the split looks for glslcCompile in them)
//   fact <title ID> <key> <name> <JSON value>       a hand-written fact of shader_facts_manual.tsv, its value as
//                                                   the split writes it (json.dumps with sorted keys, no spaces)
// Lines that start with # and empty lines are skipped.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"

namespace nxmac::shader {

struct ShaderProfile {
    std::string title_id;
    std::vector<std::pair<std::string, std::string>> groups;  // (part of a label, group)
    std::string note;
    std::int64_t key_skip_offset = 0;
    std::int64_t key_skip_length = 0;
    std::vector<std::string> modules;
    // The hand-written facts: shader key -> fact name -> its JSON text, names in byte order
    std::map<std::string, std::map<std::string, std::string>> facts;
};

class ShaderProfiles {
public:
    static absl::StatusOr<ShaderProfiles> Parse(std::string_view text);
    static absl::StatusOr<ShaderProfiles> ReadFile(const std::string& path);
    // The table built into nxmac (src/shader/profiles.tsv)
    static const ShaderProfiles& BuiltIn();

    // The profile of a base game title ID (16 hex digits, either case); NotFound names the profiles there are
    absl::StatusOr<const ShaderProfile*> Find(std::string_view title_id) const;
    const std::string& source() const { return source_; }
    std::size_t size() const { return profiles_.size(); }

private:
    std::string source_;
    std::map<std::string, ShaderProfile> profiles_;  // by upper-case title ID
};

}  // namespace nxmac::shader
