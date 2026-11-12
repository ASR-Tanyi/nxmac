// The code of a build's patches (internal to nxmac::rewriter): what nxnative's patch files hold as Python functions,
// FRAME_STEP[build ID](ctx) and STUBS[build ID](ctx), ported by hand in src/rewriter/games. A stub assembles its
// instructions into the patch area and branches to them from its sites, through this context (patch_nso.py "Frame step
// in whole vsyncs" and "Stubs"). Its code must hold nothing the rewriter would rewrite (x18, svc, tpidr, exclusive
// pairs): it is not scanned.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "image.h"

namespace nxmac::rewriter::internal {

class StubContext {
public:
    virtual ~StubContext() = default;

    // The address the next emitted word gets
    virtual Addr Here() const = 0;
    virtual void Emit(std::uint32_t word) = 0;
    // Asserts a site: its original bytes (hex digits), and that it holds no rewrite site
    virtual void Site(Addr offset, std::string_view original) = 0;
    // Writes words at a site; the change is listed with the cuts under its reason
    virtual void Put(Addr offset, const std::vector<std::uint32_t>& words, const std::string& why) = 0;
    // The data address the adrp at offset and the add, load or store right after it make, from the original text
    virtual Addr Address(Addr offset) const = 0;

    void Emit(std::initializer_list<std::uint32_t> words) {
        for (const std::uint32_t w : words) Emit(w);
    }

    // The frame step's state page (0 for STUBS): +0 float target fps, +4 the 30 fps movie flag, +0x10 float render
    // scale, +0x20 u32 step in vsyncs, +0x24 u32 a step the game fixes itself, +0x30 u64 vsyncs stepped, ...
    Addr state = 0;
    // n: the build has a render-scale stub (state +0x10) that takes scales up to n
    int render_scale = 0;
    // Launcher capabilities the stubs provide, written into the fixups as CAPABILITIES are
    std::set<std::string> capabilities;
};

using StubFunction = void (*)(StubContext&);

struct FrameStepStub {
    std::string_view build_id;
    StubFunction function;
};

// The frame-step code of a main build, and the other stub code of a module's build; nullptr when there is none ported
StubFunction FindFrameStep(std::string_view build_id);
StubFunction FindStubs(std::string_view build_id);

}  // namespace nxmac::rewriter::internal
