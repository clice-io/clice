module clice;

import :command.file_kind;
import :compile.diagnostic;
import :project.hosting;
import :project.project;
import :vfs.disk_state;

namespace clice {

std::optional<bool> Project::self_contained(const TrialKey& key, bool open) {
    auto& files = file_table;
    // An X-macro list and the like compile only inside their includer.
    if(is_context_header_path(files.resolve(key.first))) {
        return false;
    }
    auto* entry = hosting.trials.lookup(files, key);
    if(!entry) {
        return std::nullopt;
    }
    // A result the other side does not take is kept for it. The editor's
    // resolutions look at the disk through what the file table saw last, as
    // they read the buffer, not the file.
    if(open) {
        auto seen = files.disk.seen_hash(key.first);
        if(!seen || *seen != files.version(entry->result.disk).content_hash) {
            return std::nullopt;
        }
    } else if(files.check_version(entry->result.text) != vfs::DiskState::CheckResult::Fresh) {
        return std::nullopt;
    }
    return entry->result.self_contained;
}

std::size_t missing_context_errors(llvm::ArrayRef<feature::FileDiagnostic> diagnostics) {
    namespace diag = clang::diag;
    constexpr static unsigned ids[] = {
        diag::err_unknown_typename,
        diag::err_unknown_typename_suggest,
        diag::err_undeclared_var_use,
        diag::err_undeclared_var_use_suggest,
        diag::err_undeclared_use,
        diag::err_undeclared_use_suggest,
        diag::err_no_member,
        diag::err_no_member_suggest,
        diag::err_no_member_template,
        diag::err_no_member_template_suggest,
        diag::err_no_template,
        diag::err_no_template_suggest,
        diag::err_typename_nested_not_found,
        diag::err_unknown_nested_typename_suggest,
        diag::ext_implicit_function_decl_c99,
        diag::err_pp_hash_error,
        diag::err_pp_unterminated_conditional,
    };
    return ranges::count_if(diagnostics, [&](const feature::FileDiagnostic& diagnostic) {
        return diagnostic.level >= DiagnosticLevel::Error && ranges::contains(ids, diagnostic.id);
    });
}

void Project::commit_trial(const TrialKey& key,
                           llvm::ArrayRef<feature::FileDiagnostic> diagnostics,
                           VersionID text,
                           DepsSnapshot trace) {
    auto disk = current_dep(file_table, key.first);
    hosting.trials.commit(key,
                          {
                              .self_contained = missing_context_errors(diagnostics) == 0,
                              .text = text,
                              // A header gone from disk holds no result.
                              .disk = disk && disk->version.valid() ? disk->version : text,
                          },
                          std::move(trace));
    // A header found self-contained lists hosts lending the same command
    // once (list_contexts).
    context_epoch += 1;
    mark_precompiled_dirty();
}

bool Project::saved_trial(const TrialKey& key, VersionID text, bool leans) {
    auto* entry = hosting.trials.find(key);
    if(!entry) {
        return false;
    }
    mark_precompiled_dirty();
    if(entry->result.self_contained && leans) {
        hosting.trials.forget(key);
        context_epoch += 1;
        return true;
    }
    entry->result.text = text;
    entry->result.disk = text;
    return false;
}

}  // namespace clice
