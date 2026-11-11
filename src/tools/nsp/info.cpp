// nsp info: what the packages hold, and whether the keys open every NCA.
#include <algorithm>

#include "absl/strings/str_format.h"
#include "nsp/commands.h"
#include "nxmac/fs/nacp.h"
#include "nxmac/fs/romfs.h"

namespace nxmac::nsp {
namespace {

// The name and display version of a Control NCA, or why they cannot be read
std::string ControlLine(const fs::Nca& control) {
    absl::StatusOr<fs::StoragePtr> section = control.OpenSection(0);
    if (!section.ok()) return std::string(section.status().message());
    absl::StatusOr<fs::RomFs> romfs = fs::RomFs::Open(*section);
    if (!romfs.ok()) return std::string(romfs.status().message());
    for (const fs::RomFs::Entry& e : romfs->entries()) {
        if (e.path != "/control.nacp") continue;
        absl::StatusOr<std::vector<std::uint8_t>> bytes = fs::ReadBytes(**romfs->OpenFile(e), 0, e.size);
        if (!bytes.ok()) return std::string(bytes.status().message());
        absl::StatusOr<fs::ControlData> data = fs::ParseNacp(*bytes);
        if (!data.ok()) return std::string(data.status().message());
        return absl::StrFormat("%s, display version %s", data->name, data->display_version);
    }
    return "no control.nacp";
}

}  // namespace

int Info(const Context& ctx) {
    int code = kOk;
    for (const std::string& path : ctx.options.packages) {
        absl::StatusOr<OpenedPackage> p = OpenPackage(ctx.keys, path);
        if (!p.ok()) {
            ctx.err << "[error] " << p.status().message() << "\n";
            code = std::max(code, static_cast<int>(kFailed));
            continue;
        }
        ctx.out << p->name << "\n";
        for (const fs::ContentMeta& m : p->metas) {
            ctx.out << absl::StrFormat("  %s %s, version %u (%u content%s)\n", MetaTypeName(m.type),
                                       m.title_id.ToString(), m.version, m.contents.size(),
                                       m.contents.size() == 1 ? "" : "s");
        }
        bool keys_open = true;
        for (const fs::Package::Content& c : p->package.contents()) {
            const fs::NcaHeader& h = c.nca.header();
            ctx.out << absl::StrFormat("    %-11s %s  %s  %s, key generation %d%s\n", ContentTypeName(h.content_type),
                                       h.program_id.ToString(), c.file_name, Size(h.content_size), h.key_generation,
                                       h.has_rights_id() ? ", title key" : "");
            if (!c.nca.key_status().ok()) {
                ctx.out << "      [keys] " << c.nca.key_status().message() << "\n";
                keys_open = false;
                continue;
            }
            for (int i = 0; i < 4; ++i) {
                const std::optional<fs::NcaSection>& s = h.sections[i];
                if (s && (s->sparse || s->compressed)) {
                    ctx.out << absl::StrFormat("      section %d is %s: nsp cannot read it yet\n", i,
                                               s->sparse ? "sparse" : "compressed");
                }
            }
            if (h.content_type == fs::NcaContentType::kControl) ctx.out << "      " << ControlLine(c.nca) << "\n";
        }
        if (!keys_open) code = std::max(code, static_cast<int>(kMissingKeys));
    }
    return code;
}

}  // namespace nxmac::nsp
