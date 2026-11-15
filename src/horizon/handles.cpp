#include "nxmac/horizon/handles.h"

#include "absl/status/status.h"

namespace nxmac::horizon {

absl::StatusOr<std::uint32_t> HandleTable::New(ObjectType type, void* object) {
    if (type == ObjectType::kFree) return absl::InvalidArgumentError("a handle to nothing");
    for (std::uint32_t i = 1; i < kSlots; ++i) {
        if (slots_[i].type == ObjectType::kFree) {
            slots_[i] = {.type = type, .object = object};
            return kFirstHandle + i;
        }
    }
    return absl::ResourceExhaustedError("handle table full");
}

void* HandleTable::Peek(std::uint32_t handle, ObjectType type) const {
    const std::uint32_t i = handle - kFirstHandle;
    if (i == 0 || i >= kSlots || slots_[i].type != type) return nullptr;
    return slots_[i].object;
}

}  // namespace nxmac::horizon
