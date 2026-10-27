module;

#include "support/prelude.h"

module clice:project.configuration;

import :config.config;
import :index.database;

namespace clice {

/// The configuration active when nothing selects one: `default_configuration`
/// when it names a declared tag, else the first declared tag; empty when
/// the rules declare none.
llvm::StringRef fallback_configuration(const Config& config);

/// The persisted selection: the `configuration` field of `state.json` under
/// the cache directory. Empty when there is none.
std::string read_selection(llvm::StringRef cache_dir);

/// Persist `configuration` as the selection, written whole and renamed
/// into place.
std::expected<void, std::error_code> write_selection(llvm::StringRef cache_dir,
                                                     llvm::StringRef configuration);

/// Whether `name` is one of the tags the rules declare.
bool declares_configuration(const Config& config, llvm::StringRef name);

/// For the commands that run scripted (`clice index`, `lint`, `query`,
/// …): a `requested` name no rule declares is an error, not a fallback.
/// Why, naming the declared ones, unless the name is empty or declared.
std::expected<void, std::string> check_requested_configuration(const Config& config,
                                                               llvm::StringRef requested);

/// The configuration a session activates: `requested` (the command line),
/// else the persisted selection, else the fallback. A name no rule declares
/// is reported as guidance and skipped; the selection file is left as it
/// is. Without declared tags every layer is ignored and the anonymous
/// configuration (empty) is active.
std::string resolve_configuration(const Config& config, llvm::StringRef requested);

/// The index database of the build `configuration` reads: the
/// configuration's own, unless an active rule resolves symlinks
/// (ConfigRule::resolve_symlinks); then the one of the real paths its
/// databases resolve to now, or the own one of another configuration whose
/// rules resolve no symlinks and declare exactly those databases.
index::DatabaseKey database_key(const Config& config, llvm::StringRef configuration);

}  // namespace clice
