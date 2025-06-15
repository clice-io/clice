#include "Compiler/CompilationUnit.h"
#include "Feature/DocumentLink.h"
#include "Support/Ranges.h"
#include "Support/Compare.h"

namespace clice::feature {

namespace {}

DocumentLinks documentLinks(CompilationUnit& unit);

index::Shared<DocumentLinks> indexDocumentLink(CompilationUnit& unit) {
    index::Shared<DocumentLinks> result;

    for(auto& [fid, diretives]: unit.directives()) {
        for(auto& include: diretives.includes) {
            auto [_, range] = unit.decompose_range(include.fileNameRange);
            result[fid].emplace_back(range, unit.file_path(include.fid).str());
        }

        auto content = unit.file_content(fid);
        for(auto& hasInclude: diretives.hasIncludes) {
            /// If the include path is empty, skip it.
            if(hasInclude.fid.isInvalid()) {
                continue;
            }

            auto location = hasInclude.location;
            auto [_, offset] = unit.decompose_location(location);

            auto subContent = content.substr(offset);

            bool isFirst = true;
            std::uint32_t endOffset = offset;
            tokenize(subContent, [&](const clang::Token& token) {
                if(token.is(clang::tok::r_paren) || (!isFirst && token.isAtStartOfLine())) {
                    return false;
                }

                if(isFirst) {
                    isFirst = false;
                }

                endOffset = offset + token.getEndLoc().getRawEncoding() - fakeLoc.getRawEncoding();
                return true;
            });

            result[fid].emplace_back(LocalSourceRange{offset, endOffset},
                                     unit.file_path(hasInclude.fid).str());
        }
    }

    for(auto& [_, links]: result) {
        ranges::sort(links, refl::less);
    }

    return result;
}

}  // namespace clice::feature
