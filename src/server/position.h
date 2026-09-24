#pragma once

#include <optional>

/// The editor's positions as the master's buffer access takes them.

#include "syntax/token.h"

#include "kota/ipc/lsp/position.h"

namespace clice {

namespace protocol = kota::ipc::protocol;

/// Clamp a client-supplied position to the document, following LSP
/// semantics: a character beyond the line length defaults to the line end
/// (also when it lands mid-codepoint), a line beyond the document defaults
/// to the end of the content.
inline kota::ipc::lsp::LineMap::Offset clamped_offset(const kota::ipc::lsp::LineMap& map,
                                                      const protocol::Position& position) {
    if(auto offset = map.to_offset(position)) {
        return *offset;
    }
    auto starts = map.line_starts();
    if(position.line >= starts.size()) {
        return static_cast<kota::ipc::lsp::LineMap::Offset>(map.content().size());
    }
    return map.line_bounds(starts[position.line]).end;
}

/// The byte range of a request's range, both ends clamped; nullopt when
/// the range ends before it starts, which no client should send.
inline std::optional<LocalSourceRange> clamped_range(const kota::ipc::lsp::LineMap& map,
                                                     const protocol::Range& range) {
    LocalSourceRange local{clamped_offset(map, range.start), clamped_offset(map, range.end)};
    if(local.begin > local.end) {
        return std::nullopt;
    }
    return local;
}

}  // namespace clice
