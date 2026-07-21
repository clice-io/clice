#pragma once

#include <cassert>
#include <vector>

#include "syntax/token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice::testing {

/// Snapshot fixtures may bracket parts of the file with whole-line markers:
///
///     /// <snap:begin>          /// <snap:begin init>
///     ... code ...              ... code ...
///     /// <snap:end>            /// <snap:end init>
///
/// A feature's snapshot transform keeps only result entries fully contained
/// in one of the marked regions; a file without markers snapshots
/// everything. Regions may not nest, and a named end must match its begin.
inline std::vector<LocalSourceRange> extract_snap_regions(llvm::StringRef content) {
    std::vector<LocalSourceRange> regions;

    bool open = false;
    llvm::StringRef open_name;
    std::uint32_t region_begin = 0;

    std::uint32_t line_start = 0;
    llvm::StringRef rest = content;
    while(!rest.empty()) {
        auto parts = rest.split('\n');
        llvm::StringRef raw_line = parts.first;
        rest = parts.second;
        // Past-the-newline offset; for the last unterminated line this is
        // simply the end of the file.
        std::uint32_t next_line_start =
            line_start + static_cast<std::uint32_t>(raw_line.size()) + (rest.data() ? 1 : 0);

        llvm::StringRef line = raw_line.trim();
        // Any line starting with `/// <snap:` is claimed by the marker
        // grammar, so a typo fails loudly instead of being skipped.
        if(llvm::StringRef marker = line; marker.consume_front("/// <snap:")) {
            bool is_begin = marker.consume_front("begin");
            bool is_end = !is_begin && marker.consume_front("end");
            assert((is_begin || is_end) && "Expect <snap:begin ...> or <snap:end ...>.");
            [[maybe_unused]] bool closed = marker.consume_back(">");
            assert(closed && "Snap marker must end with `>`.");
            assert((marker.empty() || marker.starts_with(" ")) &&
                   "Snap marker name must be separated by a space.");
            llvm::StringRef name = marker.trim();

            if(is_begin) {
                assert(!open && "<snap:begin> may not nest.");
                open = true;
                open_name = name;
                region_begin = next_line_start;
            } else {
                assert(open && "<snap:end> without a matching <snap:begin>.");
                assert((name.empty() || name == open_name) &&
                       "<snap:end NAME> must match its <snap:begin NAME>.");
                regions.emplace_back(region_begin, line_start);
                open = false;
            }
        }

        line_start = next_line_start;
    }

    assert(!open && "Unclosed <snap:begin> at end of file.");
    return regions;
}

/// True when `range` should appear in the snapshot: always if the fixture
/// has no marked regions, otherwise only when fully contained in one.
inline bool snap_region_filter(llvm::ArrayRef<LocalSourceRange> regions, LocalSourceRange range) {
    if(regions.empty()) {
        return true;
    }
    for(const auto& region: regions) {
        if(region.begin <= range.begin && range.end <= region.end) {
            return true;
        }
    }
    return false;
}

}  // namespace clice::testing
