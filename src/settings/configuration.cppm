module clice:settings.configuration;

import :settings.settings;
import :support.prelude;

namespace clice {

/// The persisted selection: the `configuration` field of `state.json` under
/// the cache directory. Empty when there is none.
std::string read_selected_configuration(llvm::StringRef cache_dir);

/// Persist `configuration` as the selection, written whole and renamed
/// into place.
std::expected<void, std::error_code> write_selected_configuration(llvm::StringRef cache_dir,
                                                                  llvm::StringRef configuration);

/// The configuration a project runs under: `requested` (the command line),
/// else the persisted selection, else the fallback (Settings::
/// fallback_configuration); empty when the rules declare none. A requested
/// name no rule declares is an error when `strict` (the commands that run
/// scripted, where a fallback would answer for the wrong build), else
/// skipped, as a stale selection is; `issues` says so. The selection file
/// is left as it is.
std::string choose_configuration(const Settings& settings,
                                 llvm::StringRef requested,
                                 bool strict,
                                 std::vector<SettingsIssue>& issues);

}  // namespace clice
