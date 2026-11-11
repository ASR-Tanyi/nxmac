// nsp verify: every file of every NCA read through its hash checks, nothing written.
#include <algorithm>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "nsp/commands.h"
#include "nxmac/fs/partition_fs.h"
#include "nxmac/fs/romfs.h"

namespace nxmac::nsp {
namespace {

// The NCA, among all the packages, that an update's patch section lays over: the same content type and program ID, with
// that section not itself a patch
const fs::Nca* FindBase(const std::vector<OpenedPackage>& packages, const fs::Nca& patch, int index) {
    for (const OpenedPackage& p : packages) {
        for (const fs::Package::Content& c : p.package.contents()) {
            const fs::NcaHeader& h = c.nca.header();
            if (&c.nca == &patch || h.content_type != patch.header().content_type ||
                h.program_id != patch.header().program_id) {
                continue;
            }
            const std::optional<fs::NcaSection>& s = h.sections[index];
            if (s && s->encryption_type != fs::NcaEncryptionType::kAesCtrEx) return &c.nca;
        }
    }
    return nullptr;
}

// The files of a section (a RomFS's or a PFS0's), or the section whole when it is neither
absl::Status AddSection(const std::string& where, const fs::StoragePtr& section, fs::NcaFsType type,
                        std::vector<Item>* items) {
    if (type == fs::NcaFsType::kRomFs) {
        absl::StatusOr<fs::RomFs> romfs = fs::RomFs::Open(section);
        if (!romfs.ok()) return romfs.status();
        for (const fs::RomFs::Entry& e : romfs->entries()) {
            if (e.directory) continue;
            absl::StatusOr<fs::StoragePtr> file = romfs->OpenFile(e);
            if (!file.ok()) return file.status();
            items->push_back({absl::StrCat(where, ":", e.path), *std::move(file), {}});
        }
        return absl::OkStatus();
    }
    absl::StatusOr<fs::PartitionFs> pfs = fs::PartitionFs::Open(section);
    if (!pfs.ok()) {
        items->push_back({where, section, {}});
        return absl::OkStatus();
    }
    for (const fs::PartitionFs::Entry& e : pfs->entries()) {
        absl::StatusOr<fs::StoragePtr> file = pfs->OpenFile(e);
        if (!file.ok()) return file.status();
        items->push_back({absl::StrCat(where, ":/", e.name), *std::move(file), {}});
    }
    return absl::OkStatus();
}

}  // namespace

int Verify(const Context& ctx) {
    std::vector<OpenedPackage> packages;
    for (const std::string& path : ctx.options.packages) {
        absl::StatusOr<OpenedPackage> p = OpenPackage(ctx.keys, path);
        if (!p.ok()) {
            ctx.err << "[error] " << p.status().message() << "\n";
            return kFailed;
        }
        packages.push_back(*std::move(p));
    }
    std::vector<NamedNca> ncas;
    for (const OpenedPackage& p : packages) {
        for (const fs::Package::Content& c : p.package.contents()) ncas.push_back({p.name + "/" + c.file_name, &c.nca});
    }
    if (!KeysOpen(ctx, ncas)) return kMissingKeys;

    int code = kOk;
    std::vector<Item> items;
    std::vector<std::string> skipped;
    for (const OpenedPackage& p : packages) {
        for (const fs::Package::Content& c : p.package.contents()) {
            for (int i = 0; i < 4; ++i) {
                const std::optional<fs::NcaSection>& s = c.nca.header().sections[i];
                if (!s) continue;
                const std::string where = absl::StrFormat("%s/%s section %d", p.name, c.file_name, i);
                absl::StatusOr<fs::StoragePtr> section;
                if (s->encryption_type == fs::NcaEncryptionType::kAesCtrEx) {
                    const fs::Nca* base = FindBase(packages, c.nca, i);
                    if (base == nullptr) {
                        skipped.push_back(where);
                        continue;
                    }
                    section = fs::OpenPatchedSection(*base, c.nca, i);
                } else {
                    section = c.nca.OpenSection(i);
                }
                absl::Status s_open = section.status();
                if (s_open.ok()) s_open = AddSection(where, *section, s->fs_type, &items);
                if (!s_open.ok()) {
                    const bool damaged = s_open.code() == absl::StatusCode::kDataLoss;
                    const bool unread = s_open.code() == absl::StatusCode::kUnimplemented;
                    ctx.err << (damaged  ? "[damaged] "
                                : unread ? "[not checked] "
                                         : "[error] ")
                            << where << ": " << s_open.message() << "\n";
                    code = std::max(code, static_cast<int>(damaged ? kDamaged : kFailed));
                }
            }
        }
    }

    CopyResult result;
    code = std::max(code, Copy(ctx, items, &result));
    ctx.out << absl::StrFormat("checked %u files, %s in %.1f s (%.0f MB/s)\n", items.size() - result.failures.size(),
                               Size(result.bytes), result.seconds,
                               static_cast<double>(result.bytes) / 1e6 / std::max(result.seconds, 1e-3));
    for (const std::string& where : skipped) {
        ctx.out << "  not checked: " << where
                << ", an update's patch: it is checked over its base game, when the base game's NSP is given too\n";
    }
    return code;
}

}  // namespace nxmac::nsp
