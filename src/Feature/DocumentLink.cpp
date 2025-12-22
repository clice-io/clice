#include "Feature/DocumentLink.h"

#include "Compiler/CompilationUnit.h"
#include "Support/Compare.h"
#include "Support/Ranges.h"

namespace clice::feature {

namespace {}

DocumentLinks document_links(CompilationUnit& unit) {
    DocumentLinks links;

    auto& interested_diretive = unit.directives()[unit.interested_file()];
    for(auto include: interested_diretive.includes) {
        auto [_, range] = unit.decompose_range(include.filename_range);
        links.emplace_back(range, unit.file_path(include.fid).str());
    }

    auto content = unit.interested_content();
    for(auto& has_include: interested_diretive.has_includes) {
        /// If the include path is empty, skip it.
        if(has_include.fid.isInvalid()) {
            continue;
        }

        auto location = has_include.location;
        auto [_, offset] = unit.decompose_location(location);

        /// FIXME: handle incomplete code, the <> or "" may not be in pair.
        auto sub_content = content.substr(offset);
        char c = sub_content[0] == '<' ? '>' : '"';
        std::uint32_t end_offset = offset + sub_content.find_first_of(c, 1) + 1;

        links.emplace_back(LocalSourceRange{offset, end_offset},
                           unit.file_path(has_include.fid).str());
    }

    return links;
}

index::Shared<DocumentLinks> index_document_link(CompilationUnit& unit) {
    index::Shared<DocumentLinks> result;

    for(auto& [fid, diretives]: unit.directives()) {
        for(auto& include: diretives.includes) {
            auto [_, range] = unit.decompose_range(include.filename_range);
            result[fid].emplace_back(range, unit.file_path(include.fid).str());
        }

        auto content = unit.file_content(fid);
        for(auto& has_include: diretives.has_includes) {
            /// If the include path is empty, skip it.
            if(has_include.fid.isInvalid()) {
                continue;
            }

            auto location = has_include.location;
            auto [_, offset] = unit.decompose_location(location);

            /// FIXME: handle incomplete code, the <> or "" may not be in pair.
            auto sub_content = content.substr(offset);
            char c = sub_content[0] == '<' ? '>' : '"';
            std::uint32_t end_offset = offset + sub_content.find_first_of(c, 1) + 1;

            result[fid].emplace_back(LocalSourceRange{offset, end_offset},
                                     unit.file_path(has_include.fid).str());
        }
    }

    for(auto& [_, links]: result) {
        ranges::sort(links, refl::less);
    }

    return result;
}

}  // namespace clice::feature
