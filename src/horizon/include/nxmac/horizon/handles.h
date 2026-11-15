// Horizon's handles to the guest's kernel objects (nxnative's dm/host/hle_thread.c g_h, h_new, h_peek): a handle is
// 0x1000 plus its slot, the lowest free slot first, so the first object made (the main thread) is 0x1001. Each one
// holds an object of the host's and its type. Not thread-safe: the thread table's lock covers it.
#pragma once

#include <array>
#include <cstdint>

#include "absl/status/statusor.h"

namespace nxmac::horizon {

// What a handle holds (hle_thread.c O_*)
enum class ObjectType : std::uint8_t {
    kFree,
    kThread,
    kReadableEvent,  // an event's read end
    kWritableEvent,  // its write end
};

// The calling thread's pseudo-handle (H_CUR_THREAD), which the thread table resolves itself
inline constexpr std::uint32_t kCurrentThreadHandle = 0xFFFF8000;

class HandleTable {
public:
    static constexpr std::uint32_t kSlots = 4096;  // MAX_H; slot 0 is never used
    static constexpr std::uint32_t kFirstHandle = 0x1000;

    // A handle to `object` of `type` (not kFree): ResourceExhausted "handle table full" when every slot is taken
    absl::StatusOr<std::uint32_t> New(ObjectType type, void* object);
    // The object `handle` holds when it is one of `type`; null otherwise, a pseudo-handle included
    void* Peek(std::uint32_t handle, ObjectType type) const;

private:
    struct Slot {
        ObjectType type = ObjectType::kFree;
        void* object = nullptr;
    };
    std::array<Slot, kSlots> slots_{};
};

}  // namespace nxmac::horizon
