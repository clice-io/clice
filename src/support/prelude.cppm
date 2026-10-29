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

namespace ipc = kota::ipc;
namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

namespace codec = kota::codec;
namespace json = kota::codec::json;

namespace fs = kota::fs;
namespace sys = kota::sys;

}  // namespace clice
