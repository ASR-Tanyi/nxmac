#include "nxmac/fs/package.h"

#include <utility>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "nxmac/fs/partition_fs.h"
#include "nxmac/fs/ticket.h"

namespace nxmac::fs {

absl::StatusOr<Package> Package::Open(const KeySet& keys, StoragePtr storage) {
    absl::StatusOr<PartitionFs> pfs = PartitionFs::Open(std::move(storage));
    if (!pfs.ok()) return pfs.status();
    KeySet with_tickets = keys;
    for (const PartitionFs::Entry& e : pfs->entries()) {
        if (!absl::EndsWithIgnoreCase(e.name, ".tik")) continue;
        absl::StatusOr<StoragePtr> file = pfs->OpenFile(e);
        if (!file.ok()) return file.status();
        absl::StatusOr<std::vector<std::uint8_t>> bytes = ReadBytes(**file, 0, (*file)->size());
        if (!bytes.ok()) return bytes.status();
        absl::StatusOr<Ticket> ticket = ParseTicket(*bytes);
        if (!ticket.ok()) {
            return absl::InvalidArgumentError(absl::StrFormat("%s: %s", e.name, ticket.status().message()));
        }
        if (ticket->common) with_tickets.AddTitleKey(ticket->rights_id, ticket->title_key);
    }
    Package package;
    for (const PartitionFs::Entry& e : pfs->entries()) {
        if (!absl::EndsWithIgnoreCase(e.name, ".nca")) continue;
        absl::StatusOr<StoragePtr> file = pfs->OpenFile(e);
        if (!file.ok()) return file.status();
        absl::StatusOr<Nca> nca = Nca::Open(with_tickets, *std::move(file));
        if (!nca.ok()) {
            return absl::Status(nca.status().code(), absl::StrFormat("%s: %s", e.name, nca.status().message()));
        }
        package.contents_.push_back(
            {e.name, absl::AsciiStrToLower(e.name.substr(0, e.name.find('.'))), *std::move(nca)});
    }
    return package;
}

absl::StatusOr<Package> Package::OpenFile(const KeySet& keys, const std::string& path) {
    absl::StatusOr<StoragePtr> file = OpenFileStorage(path);
    if (!file.ok()) return file.status();
    absl::StatusOr<Package> package = Open(keys, *std::move(file));
    if (!package.ok()) {
        return absl::Status(package.status().code(), absl::StrFormat("%s: %s", path, package.status().message()));
    }
    return package;
}

const Nca* Package::Find(NcaContentType type, std::optional<core::TitleId> program_id) const {
    const Nca* found = nullptr;
    for (const Content& c : contents_) {
        const NcaHeader& h = c.nca.header();
        if (h.content_type != type) continue;
        if (program_id ? h.program_id == *program_id : found == nullptr || h.program_id < found->header().program_id) {
            found = &c.nca;
        }
    }
    return found;
}

absl::StatusOr<std::vector<ContentMeta>> Package::ContentMetas() const {
    std::vector<ContentMeta> metas;
    for (const Content& c : contents_) {
        if (c.nca.header().content_type != NcaContentType::kMeta) continue;
        absl::StatusOr<StoragePtr> section = c.nca.OpenSection(0);
        if (!section.ok()) {
            return absl::Status(section.status().code(),
                                absl::StrFormat("%s: %s", c.file_name, section.status().message()));
        }
        absl::StatusOr<PartitionFs> pfs = PartitionFs::Open(*std::move(section));
        if (!pfs.ok()) return pfs.status();
        for (const PartitionFs::Entry& e : pfs->entries()) {
            if (!absl::EndsWith(e.name, ".cnmt")) continue;
            absl::StatusOr<StoragePtr> file = pfs->OpenFile(e);
            if (!file.ok()) return file.status();
            absl::StatusOr<std::vector<std::uint8_t>> bytes = ReadBytes(**file, 0, (*file)->size());
            if (!bytes.ok()) return bytes.status();
            absl::StatusOr<ContentMeta> meta = ParseContentMeta(*bytes);
            if (!meta.ok()) {
                return absl::InvalidArgumentError(
                    absl::StrFormat("%s/%s: %s", c.file_name, e.name, meta.status().message()));
            }
            metas.push_back(*std::move(meta));
        }
    }
    return metas;
}

}  // namespace nxmac::fs
