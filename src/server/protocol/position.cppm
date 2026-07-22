/// Shared LSP position clamping for master-side buffer access.

export module clice:server.protocol.position;

import kota;

export namespace clice {

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

}  // namespace clice
