// The builds whose frame-step and stub code nxmac has (stubs.h): one line per build, naming the function that its
// game's file in this folder defines.
#include "games/games.h"
#include "stubs.h"

namespace nxmac::rewriter::internal {
namespace {

struct Entry {
    std::string_view build_id;
    StubFunction function;
};

constexpr Entry kFrameSteps[] = {
    {"cd57b23fa4bbad65803d9788c01821ee", games::FrameStep01007EF00011E000V190},
};

constexpr Entry kStubs[] = {
    {"", nullptr},
};

template <std::size_t N>
StubFunction Find(const Entry (&entries)[N], std::string_view build_id) {
    for (const Entry& entry : entries) {
        if (entry.function != nullptr && entry.build_id == build_id) return entry.function;
    }
    return nullptr;
}

}  // namespace

StubFunction FindFrameStep(std::string_view build_id) { return Find(kFrameSteps, build_id); }
StubFunction FindStubs(std::string_view build_id) { return Find(kStubs, build_id); }

}  // namespace nxmac::rewriter::internal
