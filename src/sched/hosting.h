#pragma once

#include <optional>
#include <vector>

#include "vfs/file_table.h"

#include "llvm/ADT/SmallVector.h"

namespace clice {

struct Workspace;

/// A translation unit standing in for a header, with the include chain
/// from it to the header.
struct Host {
    Fid file;
    std::vector<Fid> chain;
};

/// The translation units that can stand in for `header` — its includers
/// the build compiles in a language the header can be part of (a `.h`
/// in any, a `.hpp` only in C++) — best first: units whose entries come
/// from the databases the header's own rules name, then the unit sharing
/// the header's stem, its directory, and path proximity.
llvm::SmallVector<Fid> ranked_hosts(Workspace& workspace, Fid header);

/// The unit whose command a file with neither an entry nor a host borrows
/// (CommandSource::Inferred), among the units of the file's language
/// family (a `.h` matches any, a `.c` never borrows C++): one in the
/// file's directory — same stem first, then by name — else, for a
/// header, the unit whose header search directories contain it, nearest
/// directory first, else the unit closest by path. Nullopt when the
/// build has no such unit.
std::optional<Fid> command_donor(Workspace& workspace, Fid file);

/// The host a header compiles under when nothing is pinned: the first
/// ranked one with an include chain to it.
std::optional<Host> default_host(Workspace& workspace, Fid header);

}  // namespace clice
