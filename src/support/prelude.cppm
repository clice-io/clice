/// What every unit of clice reads: the standard library, the C library's
/// names past it and the third-party module, imported for clice's own units
/// alone, then the short names clice gives the namespaces it writes. Their
/// macros come with support/macros.h.
export module clice:support.prelude;

import std.compat;
import libc;
import third_party;

namespace clice {

namespace ranges = std::ranges;
namespace views = std::views;
namespace chrono = std::chrono;
namespace this_thread = std::this_thread;

namespace ipc = kotatsu::ipc;
namespace lsp = kotatsu::ipc::lsp;
namespace protocol = kotatsu::ipc::protocol;

namespace codec = kotatsu::codec;
namespace json = kotatsu::codec::json;

namespace fs = kotatsu::fs;
namespace sys = kotatsu::sys;

namespace async = kotatsu::async;

namespace deco = kotatsu::deco;
namespace meta = kotatsu::meta;
namespace option = kotatsu::option;
namespace zest = kotatsu::zest;

}  // namespace clice
