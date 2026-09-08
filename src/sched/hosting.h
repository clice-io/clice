#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "command/command.h"
#include "sched/build.h"
#include "vfs/file_table.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

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
/// in any, a `.hpp` in C++ and the languages built on it, a `.cuh` only
/// in CUDA) — best first: units whose entries come from the databases
/// the header's own rules name, then the unit sharing the header's stem,
/// its directory, and path proximity.
llvm::SmallVector<Fid> ranked_hosts(Workspace& workspace, Fid header);

/// The commands of `host` that compile `header` in a language it can be
/// part of, in the host's order: what the header may compile under, its
/// first the default. Empty when the host cannot stand in for it.
llvm::SmallVector<Candidate, 2> host_commands(Workspace& workspace, Fid header, Fid host);

/// A unit and the one of its base commands that another file borrows.
struct Lender {
    Fid unit;
    ConfigID config;
};

/// The commands the build's units can lend — every command of every
/// member, by unit path — and the header search directories they cover,
/// as indexes into `commands`. Rebuilt when Workspace::commands_epoch
/// moves, so a resolution scans no unit.
struct LenderIndex {
    struct Command {
        Lender lender;
        /// What the command compiles its unit as.
        clang::driver::types::ID language;
    };

    llvm::SmallVector<Command> commands;
    llvm::StringMap<llvm::SmallVector<std::uint32_t>> search_dirs;
    std::uint64_t epoch = 0;
};

/// The lender of a file with neither an entry nor a host
/// (CommandSource::Inferred), among the units the build compiles in a
/// language the file can be part of (a `.h` matches any, a `.c` never
/// borrows C++): one in the file's directory — same stem first, then by name — with its
/// first command; else, for a header, the unit whose command's header
/// search directories contain it, nearest directory first, with that
/// command; else the unit closest by path. Nullopt when the build has no
/// such unit.
std::optional<Lender> command_lender(Workspace& workspace, Fid file);

/// The host a header compiles under when nothing is pinned: the first
/// ranked one with an include chain to it.
std::optional<Host> default_host(Workspace& workspace, Fid header);

}  // namespace clice
