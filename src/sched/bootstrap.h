#pragma once

#include "project/load.h"

#include "llvm/ADT/StringRef.h"

namespace clice {

class IndexPump;

/// The one project loading sequence, shared by the server's initialize
/// and the batch driver so the two can never drift apart: load the
/// project (load_project), claim its index load's report into the pump
/// and seed the indexing sweep. See load_project for the flags.
ProjectLoad bootstrap_project(Project& project,
                              IndexStore& store,
                              IndexPump& pump,
                              llvm::StringRef root,
                              llvm::StringRef requested_configuration,
                              bool read_only_index = false,
                              bool scan_tree = false);

}  // namespace clice
