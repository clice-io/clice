/// What every unit of clice reads: the standard library, the C library's
/// names past it and the third-party module, imported for clice's own units
/// alone, then the short names clice gives the namespaces it uses
/// everywhere. Their macros come with support/macros.h.
export module clice:support.prelude;

import std.compat;
import libc;
import third_party;

namespace clice {

namespace ranges = std::ranges;
namespace views = std::views;

namespace ipc = kotatsu::ipc;
namespace lsp = kotatsu::ipc::lsp;
namespace protocol = kotatsu::ipc::protocol;

namespace codec = kotatsu::codec;
namespace json = kotatsu::codec::json;

namespace fs = kotatsu::fs;
namespace sys = kotatsu::sys;

namespace async = kotatsu::async;

}  // namespace clice
