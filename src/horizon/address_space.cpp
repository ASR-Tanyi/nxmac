#include "nxmac/horizon/address_space.h"

#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"

namespace nxmac::horizon {

absl::StatusOr<AddressSpaceLayout> PlanAddressSpace(const AddressSpaceProfile& profile, std::uint64_t page_size,
                                                    const char* alias_size_env) {
    const std::uint64_t page_mask = page_size - 1;
    if (profile.reserve < kFrontSize || profile.alias > profile.reserve - kFrontSize ||
        profile.stack > profile.reserve - kFrontSize - profile.alias ||
        ((profile.reserve | profile.alias | profile.stack) & page_mask) != 0) {
        return absl::InvalidArgumentError("guest address-space profile has invalid native boundaries");
    }
    std::uint64_t alias = std::max(profile.alias, kSwitchAliasSize);
    if (alias_size_env != nullptr && *alias_size_env != '\0') {
        alias = std::strtoull(alias_size_env, nullptr, 0);
        if (alias < kFrontSize || (alias & page_mask) != 0) {
            return absl::InvalidArgumentError(
                absl::StrFormat("DM_ALIAS_SIZE %s: at least 256 MB, in whole host pages", alias_size_env));
        }
    }
    AddressSpaceLayout l;
    l.size = profile.reserve - profile.alias + alias;
    l.heap = kFrontSize;
    l.heap_size = profile.reserve - profile.alias - profile.stack - kFrontSize;
    l.stack = l.heap + l.heap_size;
    l.stack_size = profile.stack;
    l.alias = l.stack + l.stack_size;
    l.alias_size = alias;
    return l;
}

absl::StatusOr<GuestAddressSpace> GuestAddressSpace::Reserve(const AddressSpaceLayout& layout) {
    void* const p = mmap(nullptr, layout.size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return absl::InternalError(
            absl::StrFormat("reserve guest AS (%#x bytes): %s", layout.size, std::strerror(errno)));
    }
    GuestAddressSpace as;
    as.base_ = static_cast<std::uint8_t*>(p);
    as.layout_ = layout;
    return as;
}

GuestAddressSpace::GuestAddressSpace(GuestAddressSpace&& other) noexcept
    : base_(std::exchange(other.base_, nullptr)), layout_(std::exchange(other.layout_, {})) {}

GuestAddressSpace& GuestAddressSpace::operator=(GuestAddressSpace&& other) noexcept {
    if (this != &other) {
        if (base_ != nullptr) munmap(base_, layout_.size);
        base_ = std::exchange(other.base_, nullptr);
        layout_ = std::exchange(other.layout_, {});
    }
    return *this;
}

GuestAddressSpace::~GuestAddressSpace() {
    if (base_ != nullptr) munmap(base_, layout_.size);
}

bool GuestAddressSpace::Contains(std::uintptr_t address) const {
    const auto b = reinterpret_cast<std::uintptr_t>(base_);
    return base_ != nullptr && address >= b && address - b < layout_.size;
}

}  // namespace nxmac::horizon
