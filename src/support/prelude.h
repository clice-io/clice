#pragma once

/// What every module unit of clice includes first: the generated prelude,
/// which imports the standard library and the third-party module and
/// replays their macros, then the short names clice gives the namespaces
/// it uses everywhere.

#include "modules/prelude.h"

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
