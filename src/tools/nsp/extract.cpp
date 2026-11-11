// nsp extract (a game with its update laid over it) and nsp dlc (add-on content): the files written where nxnative's
// tools wrote them, the RomFS as a DMROMFS1 image or a folder tree.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "nsp/commands.h"
#include "nxmac/fs/nacp.h"
#include "nxmac/fs/partition_fs.h"
#include "nxmac/fs/romfs.h"
#include "nxmac/fs/romfs_image.h"

namespace nxmac::nsp {
namespace {

// A RomFS being written: as an image, its files go into a .part file that becomes the image once all of them are in;
// as a folder tree, into their own files
struct RomFsOutput {
    std::string path;  // RomFS.img, or the RomFS/ folder
    std::string label;
    std::optional<fs::RomFsImageLayout> layout;  // set for an image
    int fd = -1;
    std::size_t first_item = 0;
    std::size_t items = 0;
    std::uint64_t bytes = 0;
};

RomFsOutput MakeOutput(std::string path, std::string label) {
    RomFsOutput output;
    output.path = std::move(path);
    output.label = std::move(label);
    return output;
}

std::string PartPath(const std::string& image) { return image + ".part"; }

// Adds the files of romfs (nullptr: an empty RomFS) to items, written to output->path
absl::Status AddRomFs(const fs::RomFs* romfs, bool as_folder, RomFsOutput* output, std::vector<Item>* items) {
    static const std::vector<fs::RomFs::Entry> kEmpty;
    const std::vector<fs::RomFs::Entry>& entries = romfs != nullptr ? romfs->entries() : kEmpty;
    output->first_item = items->size();
    if (as_folder) {
        std::error_code ec;
        std::filesystem::create_directories(output->path, ec);
        if (ec) return absl::InternalError(absl::StrCat("creating ", output->path, ": ", ec.message()));
        for (const fs::RomFs::Entry& e : entries) {
            for (std::string_view part : absl::StrSplit(std::string_view(e.path).substr(1), '/')) {
                if (!SafeName(part)) {
                    return absl::InvalidArgumentError(absl::StrCat("RomFS name nsp will not write: ", e.path));
                }
            }
            const std::string path = output->path + e.path;
            if (e.directory) {
                std::filesystem::create_directories(path, ec);
                if (ec) return absl::InternalError(absl::StrCat("creating ", path, ": ", ec.message()));
                continue;
            }
            if (absl::Status s = CreateFile(path, e.size); !s.ok()) return s;
            absl::StatusOr<fs::StoragePtr> file = romfs->OpenFile(e);
            if (!file.ok()) return file.status();
            items->push_back({output->label + e.path, *std::move(file), {Destination::Kind::kFile, path, -1, 0}});
            output->bytes += e.size;
        }
    } else {
        absl::StatusOr<fs::RomFsImageLayout> layout = fs::RomFsImageLayout::Plan(entries);
        if (!layout.ok()) return layout.status();
        output->layout = *std::move(layout);
        const std::string part = PartPath(output->path);
        if (absl::Status s = CreateFile(part, output->layout->size()); !s.ok()) return s;
        output->fd = ::open(part.c_str(), O_WRONLY | O_CLOEXEC);
        if (output->fd < 0) return absl::ErrnoToStatus(errno, absl::StrCat("opening ", part));
        for (const fs::RomFsImageEntry& e : output->layout->entries()) {
            if (e.directory) continue;
            absl::StatusOr<fs::StoragePtr> file = romfs->OpenFile(entries[e.source]);
            if (!file.ok()) return file.status();
            items->push_back(
                {output->label + e.path, *std::move(file), {Destination::Kind::kSharedFile, "", output->fd, e.a}});
            output->bytes += e.b;
        }
    }
    output->items = items->size() - output->first_item;
    return absl::OkStatus();
}

// An image becomes RomFS.img when all its files went in: the index and the header go last, so a stopped run leaves no
// image that looks whole; otherwise its .part file is removed
absl::Status FinishRomFs(RomFsOutput* output, const CopyResult& result) {
    if (!output->layout) return absl::OkStatus();
    const bool complete = std::none_of(result.failures.begin(), result.failures.end(), [&](const Failure& f) {
        return f.item >= output->first_item && f.item < output->first_item + output->items;
    });
    const std::string part = PartPath(output->path);
    absl::Status s;
    if (complete) {
        const std::vector<std::uint8_t> index = output->layout->Index();
        s = WriteAt(output->fd, index, output->layout->index_offset(), part);
        if (s.ok()) s = WriteAt(output->fd, output->layout->Header(), 0, part);
    }
    ::close(output->fd);
    output->fd = -1;
    if (!complete || !s.ok()) {
        std::remove(part.c_str());
        return s;
    }
    if (std::rename(part.c_str(), output->path.c_str()) != 0) {
        return absl::ErrnoToStatus(errno, absl::StrCat("renaming ", part));
    }
    return absl::OkStatus();
}

// A RomFS whose files will not be copied after all: its .part file goes
void Abandon(RomFsOutput* output) {
    if (output->fd < 0) return;
    ::close(output->fd);
    output->fd = -1;
    std::remove(PartPath(output->path).c_str());
}

std::string Summary(const RomFsOutput& output) {
    if (output.layout) {
        return absl::StrFormat("%s: %u files, %s, an image of %u entries", output.path, output.items,
                               Size(output.bytes), output.layout->entries().size());
    }
    return absl::StrFormat("%s: %u files, %s", output.path, output.items, Size(output.bytes));
}

// The whole file at path in romfs
absl::StatusOr<std::vector<std::uint8_t>> ReadRomFsFile(const fs::RomFs& romfs, std::string_view path) {
    for (const fs::RomFs::Entry& e : romfs.entries()) {
        if (e.directory || e.path != path) continue;
        absl::StatusOr<fs::StoragePtr> file = romfs.OpenFile(e);
        if (!file.ok()) return file.status();
        return fs::ReadBytes(**file, 0, e.size);
    }
    return absl::NotFoundError(absl::StrCat("no ", path));
}

absl::Status WriteFile(const std::string& path, std::span<const std::uint8_t> bytes) {
    if (absl::Status s = CreateFile(path, bytes.size()); !s.ok()) return s;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return absl::ErrnoToStatus(errno, absl::StrCat("opening ", path));
    absl::Status s = WriteAt(fd, bytes, 0, path);
    ::close(fd);
    return s;
}

// The control data: control.nacp and the icon written, the name and display version printed
int WriteControl(const Context& ctx, const fs::Nca* base, const fs::Nca* update, const std::string& out) {
    const fs::Nca* control = update != nullptr ? update : base;
    if (control == nullptr) {
        ctx.out << "no control data\n";
        return kOk;
    }
    absl::StatusOr<fs::StoragePtr> section =
        update != nullptr && base != nullptr ? fs::OpenPatchedSection(*base, *update, 0) : control->OpenSection(0);
    absl::StatusOr<fs::RomFs> romfs =
        section.ok() ? fs::RomFs::Open(*section) : absl::StatusOr<fs::RomFs>(section.status());
    absl::StatusOr<std::vector<std::uint8_t>> nacp =
        romfs.ok() ? ReadRomFsFile(*romfs, "/control.nacp") : absl::StatusOr<std::vector<std::uint8_t>>(romfs.status());
    absl::Status s = nacp.status();
    if (s.ok()) s = WriteFile(out + "/control.nacp", *nacp);
    if (s.ok()) {
        if (absl::StatusOr<fs::ControlData> data = fs::ParseNacp(*nacp); data.ok()) {
            ctx.out << "name: " << data->name << "\ndisplay version: " << data->display_version << "\n";
        }
        bool icon = false;
        for (std::string_view language : fs::kIconLanguages) {
            absl::StatusOr<std::vector<std::uint8_t>> jpeg =
                ReadRomFsFile(*romfs, absl::StrCat("/icon_", language, ".dat"));
            if (jpeg.status().code() == absl::StatusCode::kNotFound) continue;
            s = jpeg.status();
            if (s.ok()) s = WriteFile(out + "/icon.jpg", *jpeg);
            if (s.ok()) ctx.out << absl::StrFormat("icon: %s, %s\n", language, Size(jpeg->size()));
            icon = true;
            break;
        }
        if (!icon && s.ok()) ctx.out << "no icon in the control data\n";
    }
    if (s.ok()) return kOk;
    const bool damaged = s.code() == absl::StatusCode::kDataLoss;
    ctx.err << (damaged ? "[damaged] " : "[error] ") << "control data: " << s.message() << "\n";
    return damaged ? kDamaged : kFailed;
}

int Fail(const Context& ctx, const std::string& what, const absl::Status& s) {
    const bool damaged = s.code() == absl::StatusCode::kDataLoss;
    ctx.err << (damaged ? "[damaged] " : "[error] ") << what << ": " << s.message() << "\n";
    return damaged ? kDamaged : kFailed;
}

}  // namespace

int Extract(const Context& ctx) {
    std::vector<OpenedPackage> packages;
    for (const std::string& path : ctx.options.packages) {
        absl::StatusOr<OpenedPackage> p = OpenPackage(ctx.keys, path);
        if (!p.ok()) return Fail(ctx, path, p.status());
        packages.push_back(*std::move(p));
    }
    // Told apart by their content metas
    const OpenedPackage* game = nullptr;
    const OpenedPackage* update = nullptr;
    for (const OpenedPackage& p : packages) {
        const bool is_game = p.Holds(fs::ContentMetaType::kApplication);
        const bool is_update = p.Holds(fs::ContentMetaType::kPatch);
        if (is_game && is_update) {
            ctx.err << "[error] " << p.name << " holds the game and its update together: nsp does not read that yet\n";
            return kFailed;
        }
        const OpenedPackage*& slot = is_game ? game : update;
        if (!is_game && !is_update) {
            ctx.err << "[error] " << p.name << " is neither a game nor an update\n";
            return kFailed;
        }
        if (slot != nullptr) {
            ctx.err << "[error] " << slot->name << " and " << p.name << " are both " << (is_game ? "games" : "updates")
                    << "\n";
            return kFailed;
        }
        slot = &p;
    }
    if (game == nullptr) {
        ctx.err << "[error] no game among the packages: an update is extracted with its game\n";
        return kFailed;
    }
    const fs::Nca* program = game->package.Find(fs::NcaContentType::kProgram, ctx.options.program);
    if (program == nullptr) {
        ctx.err << "[error] " << game->name << " has no Program NCA"
                << (ctx.options.program ? " of " + ctx.options.program->ToString() : "") << "\n";
        return kFailed;
    }
    const core::TitleId id = program->header().program_id;
    const fs::Nca* update_program =
        update != nullptr ? update->package.Find(fs::NcaContentType::kProgram, id) : nullptr;
    if (update != nullptr && update_program == nullptr) {
        ctx.err << "[error] " << update->name << " has no Program NCA of " << id.ToString()
                << ": not this game's update\n";
        return kFailed;
    }
    const fs::Nca* game_control = game->package.Find(fs::NcaContentType::kControl, id);
    const fs::Nca* update_control =
        update != nullptr ? update->package.Find(fs::NcaContentType::kControl, id) : nullptr;
    if (!KeysOpen(ctx, {{game->name, program},
                        {update != nullptr ? update->name : "", update_program},
                        {game->name, game_control},
                        {update != nullptr ? update->name : "", update_control}})) {
        return kMissingKeys;
    }

    ctx.out << "game " << id.ToString();
    if (update != nullptr) {
        for (const fs::ContentMeta& m : update->metas) {
            if (m.type == fs::ContentMetaType::kPatch) ctx.out << ", update version " << m.version;
        }
    }
    ctx.out << "\n";
    const std::string out = ctx.options.out;
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    if (ec) return Fail(ctx, out, absl::InternalError(ec.message()));
    int code = WriteControl(ctx, game_control, update_control, out);

    // ExeFS: whole in the update when there is one
    std::vector<Item> items;
    absl::StatusOr<fs::StoragePtr> code_section =
        update_program != nullptr ? fs::OpenPatchedSection(*program, *update_program, fs::kProgramCodeSection)
                                  : program->OpenSection(fs::kProgramCodeSection);
    absl::StatusOr<fs::PartitionFs> exefs = code_section.ok() ? fs::PartitionFs::Open(*code_section)
                                                              : absl::StatusOr<fs::PartitionFs>(code_section.status());
    if (!exefs.ok()) return std::max(code, Fail(ctx, "ExeFS", exefs.status()));
    std::uint64_t exefs_bytes = 0;
    for (const fs::PartitionFs::Entry& e : exefs->entries()) {
        if (!SafeName(e.name)) return std::max(code, Fail(ctx, "ExeFS", absl::InvalidArgumentError("name: " + e.name)));
        absl::StatusOr<fs::StoragePtr> file = exefs->OpenFile(e);
        if (!file.ok()) return std::max(code, Fail(ctx, "ExeFS/" + e.name, file.status()));
        const std::string path = out + "/ExeFS/" + e.name;
        if (absl::Status s = CreateFile(path, e.size); !s.ok()) return std::max(code, Fail(ctx, path, s));
        // An NSO module's build ID, which nxnative's profiles key their patches by
        std::string build_id;
        if (absl::StatusOr<std::vector<std::uint8_t>> h =
                fs::ReadBytes(**file, 0, std::min<std::uint64_t>(e.size, 0x50));
            h.ok() && h->size() == 0x50 && std::equal(h->begin(), h->begin() + 4, "NSO0")) {
            build_id = absl::StrCat(", build id ", absl::BytesToHexString(std::string_view(
                                                       reinterpret_cast<const char*>(h->data() + 0x40), 16)));
        }
        ctx.out << absl::StrFormat("ExeFS/%s: %s%s\n", e.name, Size(e.size), build_id);
        items.push_back({"ExeFS/" + e.name, *std::move(file), {Destination::Kind::kFile, path, -1, 0}});
        exefs_bytes += e.size;
    }

    // RomFS: the update's data section over the game's; a program without one (all its assets in its code) gets an
    // empty one
    std::optional<fs::RomFs> romfs;
    if (program->has_section(fs::kProgramDataSection) ||
        (update_program != nullptr && update_program->has_section(fs::kProgramDataSection))) {
        absl::StatusOr<fs::StoragePtr> data =
            update_program != nullptr ? fs::OpenPatchedSection(*program, *update_program, fs::kProgramDataSection)
                                      : program->OpenSection(fs::kProgramDataSection);
        absl::StatusOr<fs::RomFs> opened =
            data.ok() ? fs::RomFs::Open(*data) : absl::StatusOr<fs::RomFs>(data.status());
        if (!opened.ok()) return std::max(code, Fail(ctx, "RomFS", opened.status()));
        romfs = *std::move(opened);
    } else {
        ctx.out << "the program has no data section: an empty RomFS\n";
    }
    RomFsOutput output = MakeOutput(out + (ctx.options.romfs_dir ? "/RomFS" : "/RomFS.img"), "RomFS");
    if (absl::Status s = AddRomFs(romfs ? &*romfs : nullptr, ctx.options.romfs_dir, &output, &items); !s.ok()) {
        Abandon(&output);
        return std::max(code, Fail(ctx, "RomFS", s));
    }

    CopyResult result;
    code = std::max(code, Copy(ctx, items, &result));
    if (absl::Status s = FinishRomFs(&output, result); !s.ok()) code = std::max(code, Fail(ctx, output.path, s));
    ctx.out << absl::StrFormat("%s/ExeFS: %u files, %s\n", out, exefs->entries().size(), Size(exefs_bytes));
    ctx.out << Summary(output) << (result.failures.empty() ? "" : " (not written: files failed)") << "\n";
    ctx.out << absl::StrFormat("read %s in %.1f s (%.0f MB/s)\n", Size(result.bytes), result.seconds,
                               static_cast<double>(result.bytes) / 1e6 / std::max(result.seconds, 1e-3));
    return code;
}

int Dlc(const Context& ctx) {
    int code = kOk;
    std::vector<OpenedPackage> packages;
    for (const std::string& path : ctx.options.packages) {
        absl::StatusOr<OpenedPackage> p = OpenPackage(ctx.keys, path);
        if (!p.ok()) return Fail(ctx, path, p.status());
        packages.push_back(*std::move(p));
    }
    // Each add-on content's data NCA, by the title IDs its content metas name
    std::vector<std::pair<const OpenedPackage*, const fs::Nca*>> contents;
    for (const OpenedPackage& p : packages) {
        absl::flat_hash_set<core::TitleId> ids;
        for (const fs::ContentMeta& m : p.metas) {
            if (m.type == fs::ContentMetaType::kAddOnContent) ids.insert(m.title_id);
        }
        const std::size_t before = contents.size();
        for (const fs::Package::Content& c : p.package.contents()) {
            const fs::NcaContentType type = c.nca.header().content_type;
            if ((type == fs::NcaContentType::kPublicData || type == fs::NcaContentType::kData) &&
                ids.contains(c.nca.header().program_id)) {
                contents.emplace_back(&p, &c.nca);
            }
        }
        if (contents.size() == before) {
            ctx.err << "[error] " << p.name << " holds no add-on content\n";
            return kFailed;
        }
    }
    std::vector<NamedNca> ncas;
    ncas.reserve(contents.size());
    for (const auto& [p, nca] : contents) ncas.push_back({p->name, nca});
    if (!KeysOpen(ctx, ncas)) return kMissingKeys;

    std::error_code ec;
    std::filesystem::create_directories(ctx.options.out, ec);
    if (ec) return Fail(ctx, ctx.options.out, absl::InternalError(ec.message()));
    std::vector<Item> items;
    std::vector<fs::RomFs> romfses;
    romfses.reserve(contents.size());
    std::vector<RomFsOutput> outputs;
    outputs.reserve(contents.size());
    for (const auto& [p, nca] : contents) {
        const core::TitleId id = nca->header().program_id;
        const std::string index = std::to_string(id.value() & 0xfff);
        absl::StatusOr<fs::StoragePtr> data = nca->OpenSection(0);
        absl::StatusOr<fs::RomFs> romfs = data.ok() ? fs::RomFs::Open(*data) : absl::StatusOr<fs::RomFs>(data.status());
        absl::Status s = romfs.status();
        if (s.ok()) {
            romfses.push_back(*std::move(romfs));
            outputs.push_back(
                MakeOutput(ctx.options.out + "/" + index + (ctx.options.romfs_dir ? "" : ".img"), id.ToString()));
            s = AddRomFs(&romfses.back(), ctx.options.romfs_dir, &outputs.back(), &items);
        }
        if (!s.ok()) {
            for (RomFsOutput& output : outputs) Abandon(&output);
            return std::max(code, Fail(ctx, id.ToString(), s));
        }
        ctx.out << "add-on content " << id.ToString() << " (" << p->name << ")\n";
    }
    CopyResult result;
    code = std::max(code, Copy(ctx, items, &result));
    for (RomFsOutput& output : outputs) {
        if (absl::Status s = FinishRomFs(&output, result); !s.ok()) code = std::max(code, Fail(ctx, output.path, s));
        ctx.out << Summary(output) << "\n";
    }
    ctx.out << absl::StrFormat("read %s in %.1f s (%.0f MB/s)\n", Size(result.bytes), result.seconds,
                               static_cast<double>(result.bytes) / 1e6 / std::max(result.seconds, 1e-3));
    return code;
}

}  // namespace nxmac::nsp
