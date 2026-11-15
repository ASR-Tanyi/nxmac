// The guest's address space: one block the host reserves, inaccessible until something is mapped in it, for what
// Horizon gives an application. In it, in this order: 256 MB in front, the heap region, the stack region, then the
// alias region. GetInfo reports the block as the ASLR region and the regions in it; SetHeapSize, MapMemory and
// MapPhysicalMemory open their pages in it. The alias region is the Switch's 64 GB on the Mac (or the profile's
// mem_alias when larger, or DM_ALIAS_SIZE): games lay their regions out for that much, and it is address space, not
// memory (the application pool bounds what is mapped). It comes last because a Switch's addresses stay below 2^39
// and a 64 GB region cannot: the block usually crosses 2^39 past the heap and the stacks. A port of nxnative's
// dm/host/hle_svc.c mem_init_once. Not ported yet: the alias region's page bitmaps (MapPhysicalMemory), the guest
// memory in files (DM_GUEST_FILE) and the diagnostic threads (DM_ALIAS_WINDOWS, DM_REGIONS).
#pragma once

#include <cstddef>
#include <cstdint>

#include "absl/status/statusor.h"

namespace nxmac::horizon {

inline constexpr std::uint64_t kSwitchAliasSize = 0x1000000000;  // the Switch's alias region, 64 GB
inline constexpr std::uint64_t kFrontSize = 0x10000000;          // the 256 MB in front of the heap

// The game profile's sizes (game.tsv mem_reserve, mem_alias, mem_stack)
struct AddressSpaceProfile {
    std::uint64_t reserve = 0;  // the Switch-side block: the front, the heap, the stacks and the profile's alias region
    std::uint64_t alias = 0;
    std::uint64_t stack = 0;
};

// The block's partition, as offsets from its start
struct AddressSpaceLayout {
    std::uint64_t size = 0;  // the whole block: the profile's reserve, less its alias region, plus the one in use
    std::uint64_t heap = 0;
    std::uint64_t heap_size = 0;  // what is left of the profile's reserve
    std::uint64_t stack = 0;
    std::uint64_t stack_size = 0;
    std::uint64_t alias = 0;
    std::uint64_t alias_size = 0;
};

// The partition for `profile` on a host with `page_size`-byte pages. alias_size_env is DM_ALIAS_SIZE's text (null or
// empty when unset), the alias region's size in bytes. InvalidArgument, with nxnative's message, when the profile's
// sizes do not fit in its reserve or are not whole host pages, or when DM_ALIAS_SIZE is under 256 MB or not whole
// host pages.
absl::StatusOr<AddressSpaceLayout> PlanAddressSpace(const AddressSpaceProfile& profile, std::uint64_t page_size,
                                                    const char* alias_size_env);

// The block reserved (no access), released when destroyed (nxhost keeps it for the process's life)
class GuestAddressSpace {
public:
    // Internal when the system refuses the reservation
    static absl::StatusOr<GuestAddressSpace> Reserve(const AddressSpaceLayout& layout);

    GuestAddressSpace() = default;
    GuestAddressSpace(GuestAddressSpace&& other) noexcept;
    GuestAddressSpace& operator=(GuestAddressSpace&& other) noexcept;
    GuestAddressSpace(const GuestAddressSpace&) = delete;
    GuestAddressSpace& operator=(const GuestAddressSpace&) = delete;
    ~GuestAddressSpace();

    const AddressSpaceLayout& layout() const { return layout_; }
    std::uint8_t* base() const { return base_; }  // GetInfo's ASLR region: the whole block
    std::uint64_t size() const { return layout_.size; }
    std::uint8_t* heap() const { return At(layout_.heap); }
    std::uint8_t* stack() const { return At(layout_.stack); }
    std::uint8_t* alias() const { return At(layout_.alias); }

    // Whether `address` is inside the block (dm_guest_as_contains)
    bool Contains(std::uintptr_t address) const;

private:
    std::uint8_t* At(std::uint64_t offset) const { return base_ != nullptr ? base_ + offset : nullptr; }

    std::uint8_t* base_ = nullptr;
    AddressSpaceLayout layout_;
};

}  // namespace nxmac::horizon
