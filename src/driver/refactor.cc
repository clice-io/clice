#include <string>
#include <vector>

#include "driver/driver.h"
#include "driver/query_support.h"
#include "project/build.h"
#include "project/open_index.h"
#include "server/query_commands.h"
#include "vfs/file_system.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"

namespace clice::driver {

using kota::deco::decl::KVStyle;

namespace {

struct RefactorOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoInput(meta_var = "<ACTION>", help = "Refactoring to run: rename", required = false)
    <std::vector<std::string>> inputs;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (default: current directory)",
           required = false)
    <std::string> workspace;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Build configuration to read, one of the tags declared on rules "
               "(default: the selected one, else default_configuration)",
           required = false)
    <std::string> configuration;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "File of the symbol (relative to the workspace)",
           required = false)
    <std::string> path;

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "Symbol name", required = false)
    <std::string> name;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Symbol id (the #<hex> of an earlier answer)",
           required = false)
    <std::string> symbol;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "1-based line, with --path, naming the symbol defined there",
           required = false)
    <int> line;

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "rename: the new name", required = false)
    <std::string> to;

    DecoFlag(names = {"--dry-run"},
             help = "Answer with the planned edits without writing them",
             required = false)
    dry_run;

    DecoFlag(names = {"--fresh"},
             help =
                 "Reindex the files whose content changed since they were indexed before "
                 "planning, through the running server or a batch run of this command",
             required = false)
    fresh;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off (default: warn)",
           required = false)
    <std::string> log_level;
};

auto make_command() {
    return kota::deco::cli::command<RefactorOptions>("clice refactor <ACTION> [OPTIONS]");
}

/// Write the plan's edits, every file's new text computed before the
/// first is written: a file whose text moved on since it was indexed
/// stops the whole rename, never half of it. Files are rewritten in
/// place, keeping their permissions, owner and links.
std::expected<void, std::string> apply(FileTable& files,
                                       const index::RenamePlan& plan,
                                       llvm::StringRef new_name) {
    std::vector<std::pair<std::string, std::string>> written;
    llvm::DenseSet<Fid> seen;
    for(auto& edit: plan.edits) {
        auto file = edit.site.file;
        if(!seen.insert(file).second) {
            continue;
        }
        auto path = files.resolve(file).str();
        auto bytes = vfs::read(path, vfs::Read::Bytes);
        if(!bytes) {
            return std::unexpected(
                std::format("cannot read {}: {}", edit.site.path, bytes.error().message()));
        }
        auto content = (*bytes)->getBuffer();
        auto text = vfs::without_bom(content);
        auto renamed = index::apply_rename(text, plan, file, new_name);
        if(!renamed) {
            return std::unexpected(
                std::format("{} changed since it was indexed; run with --fresh", edit.site.path));
        }
        written.emplace_back(path,
                             content.take_front(content.size() - text.size()).str() + *renamed);
    }
    for(std::size_t i = 0; i < written.size(); i += 1) {
        auto& [path, text] = written[i];
        if(auto error = vfs::write(path, text)) {
            std::string message = std::format("cannot write {}: {}", path, error.message());
            if(i != 0) {
                std::vector<std::string> done;
                for(auto& entry: llvm::ArrayRef(written).take_front(i)) {
                    done.push_back(entry.first);
                }
                message +=
                    std::format("; the rename stopped after rewriting {}", llvm::join(done, ", "));
            }
            return std::unexpected(std::move(message));
        }
    }
    return {};
}

int run_rename(const RefactorOptions& opts, const char* self_path) {
    if(!opts.to) {
        print_json(Failure{.error = "--to names the new name"});
        return 1;
    }
    auto spelling = workspace_spelling(opts.workspace.value_or(""));
    CanonicalPath root(spelling);
    auto configuration = opts.configuration.value_or("");
    std::vector<std::string> failed;
    if(opts.fresh) {
        auto refreshed = refresh(spelling, configuration, self_path);
        if(!refreshed) {
            print_json(Failure{.error = refreshed.error()});
            return 1;
        }
        failed = std::move(*refreshed);
    }

    // The build tells a unit the index never reached from a file outside
    // the build, so the writer's load restores it.
    FileTable files;
    files.spell_root(spelling);
    Project project{files};
    CommandResolver commands{project};
    ContextsBlob saved;
    EditorContext contexts{project, commands, saved};
    auto loaded = load_index(project, commands, root, configuration, /*with_build=*/true);
    if(!loaded) {
        print_json(Failure{
            .error =
                "the index could not be opened (the log above says why); run `clice index` or pass --fresh"});
        return 1;
    }
    saved.bytes = std::move(loaded->contexts);
    contexts.load();

    index::FreshnessGate gate(files, {.check_disk = true});
    index::IndexQuery index_query(project.project_index, files, &gate, nullptr);
    query::Context ctx{.project = project, .contexts = contexts, .query = index_query};
    auto absolute = opts.path ? inspected_path(project, *opts.path) : Spelling();

    auto stale = [&](llvm::ArrayRef<std::string> listed) {
        std::vector<std::string> all(listed.begin(), listed.end());
        for(auto file: gate.withheld()) {
            all.emplace_back(files.display(file));
        }
        all.insert(all.end(), ctx.unindexed.begin(), ctx.unindexed.end());
        all.insert(all.end(), failed.begin(), failed.end());
        for(auto unit: loaded->dropped) {
            all.emplace_back(files.display(unit));
        }
        std::ranges::sort(all);
        auto duplicates = std::ranges::unique(all);
        all.erase(duplicates.begin(), duplicates.end());
        return all;
    };

    auto locator = locator_of(opts, absolute);
    if(!locator) {
        print_json(Failure{.error = locator.error(), .stale = stale({})});
        return 1;
    }

    std::vector<std::string> sources;
    for(auto& source:
        workspace_sources(root,
                          CanonicalPath(Spelling::absolute(project.config.project.cache_dir)))) {
        sources.push_back(source.str());
    }
    auto read = [](llvm::StringRef path) -> std::optional<std::string> {
        auto text = vfs::read(path);
        return text ? std::optional((*text)->getBuffer().str()) : std::nullopt;
    };
    auto planned = query::rename(
        ctx,
        std::move(*locator),
        *opts.to,
        {.files = sources, .read = read, .units_pending = query::units_pending(project)});
    if(!planned) {
        print_json(Failure{.error = planned.error(), .stale = stale({})});
        return 1;
    }

    auto& result = planned->result;
    bool blocked = planned->plan.blocked();
    if(!blocked && !opts.dry_run && !planned->plan.edits.empty()) {
        if(auto applied = apply(files, planned->plan, *opts.to); !applied) {
            result.conflicts.push_back(applied.error());
            blocked = true;
        } else {
            result.applied = true;
        }
    }
    print_json(Answer{.result = std::move(result), .stale = stale(planned->plan.stale)});
    return blocked ? 1 : 0;
}

}  // namespace

void add_refactor(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code, self_path](RefactorOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           if(!apply_log_level(opts.log_level.value_or("warn")))
               return;
           logging::stderr_logger("refactor", logging::options);
           auto action = opts.inputs && opts.inputs->size() == 1 ? opts.inputs->front() : "";
           if(action != "rename") {
               print_json(Failure{.error = action.empty()
                                               ? "name the refactoring to run: rename"
                                               : std::format("unknown refactoring '{}'", action)});
               exit_code = 1;
               return;
           }
           exit_code = run_rename(opts, self_path);
       })
        .on_error([](auto err) { print_json(Failure{.error = err.message}); });

    root.add({.name = "refactor",
              .description = "Rewrite the workspace's sources through the persisted index"},
             std::move(cmd));
}

}  // namespace clice::driver
