#pragma once

#include <vector>

#include "support/logging.h"
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
/// everything. Regions may not nest, and an end marker name (including
/// its absence) must match its begin exactly.
/// Malformed markers abort in every build mode so that typos fail loudly
/// instead of silently producing misleading snapshots.
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
        // grammar.
        if(llvm::StringRef marker = line; marker.consume_front("/// <snap:")) {
            bool is_begin = marker.consume_front("begin");
            bool is_end = !is_begin && marker.consume_front("end");
            if(!is_begin && !is_end) {
                LOG_FATAL("Expect <snap:begin ...> or <snap:end ...>, got: {}", line);
            }
            if(!marker.consume_back(">")) {
                LOG_FATAL("Snap marker must end with `>`: {}", line);
            }
            if(!marker.empty() && !marker.starts_with(" ")) {
                LOG_FATAL("Snap marker name must be separated by a space: {}", line);
            }
            llvm::StringRef name = marker.trim();

            if(is_begin) {
                if(open) {
                    LOG_FATAL("<snap:begin> may not nest.");
                }
                open = true;
                open_name = name;
                region_begin = next_line_start;
            } else {
                if(!open) {
                    LOG_FATAL("<snap:end> without a matching <snap:begin>.");
                }
                if(name != open_name) {
                    LOG_FATAL("<snap:end {}> does not match <snap:begin {}>.", name, open_name);
                }
                regions.emplace_back(region_begin, line_start);
                open = false;
            }
        }

        line_start = next_line_start;
    }

    if(open) {
        LOG_FATAL("Unclosed <snap:begin> at end of file.");
    }
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
