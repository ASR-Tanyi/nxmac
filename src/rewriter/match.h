// Function matching for patches that carry across game updates (nxnative's dm/tools/nso_match.py), internal to
// nxmac::rewriter. A fingerprint covers a whole unwind function (its internal branches, constants and registers) and
// the bodies, or imported names, of its direct callees; only relocation addresses are left out. A rule finds exactly
// one function with its assumptions checked, or none: no fuzzy matching and no guessed boundaries.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "disasm.h"
#include "image.h"
#include "nxmac/rewriter/patches.h"

namespace nxmac::rewriter::internal {

class MatchImage {
public:
    // MatchError when the module's function boundaries cannot be read
    MatchImage(Bytes image, Addr text_end, const Disassembler& md);

    // The fingerprint of the function at start (64 hex digits); MatchError for code it cannot vouch for
    const std::string& Fingerprint(Addr start);
    // The start of the one function of this size with one of these fingerprints; MatchError unless exactly one
    Addr Locate(Addr size, const std::vector<std::string>& fingerprints);
    // The cuts of a rule at module offsets: every function of it matched exactly once, at distinct places
    std::vector<Cut> MatchPatch(const MatchRule& rule);

    const std::map<Addr, Addr>& functions() const { return functions_; }

private:
    struct Body {
        std::string digest;
        std::vector<Range> branches;  // (offset in the function, target) of the branches that leave it
    };
    const Body& BodyOf(Addr start);
    const std::map<Addr, std::string>& ImportSlots();
    const std::string* PltImport(Addr target);
    std::string Callee(Addr target);  // as JSON
    std::vector<Cut> Match(const FunctionRule& rule);

    Bytes image_;
    Addr text_end_;
    const Disassembler& md_;
    std::map<Addr, Addr> functions_;  // start -> end
    std::map<Addr, Body> bodies_;
    std::map<Addr, std::string> fingerprints_;
    std::optional<std::map<Addr, std::string>> imports_;
};

// What became of a patch: its cut offsets, or none with the reason
struct MatchOutcome {
    std::string name;
    std::optional<std::vector<Addr>> offsets;
    std::string why;
};

// The verified cuts of rules, without changing the image; a patch that is missing or ambiguous is reported (a red line
// when red) and left out. log gets the report lines, outcomes one entry per patch. RewriteError when patches overlap.
std::vector<Cut> Resolve(Bytes image, Addr text_end, const std::vector<MatchRule>* rules, const Disassembler& md,
                         bool red, std::vector<std::string>* log, std::vector<MatchOutcome>* outcomes);

// As Python's repr() of a string, for the report lines
std::string PyRepr(std::string_view text);

}  // namespace nxmac::rewriter::internal
