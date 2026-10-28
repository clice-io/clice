module;

#include "support/prelude.h"

module clice;

import :config.config;
import :driver.driver;
import :feature.rename;
import :index.symbol_query;
import :project.configuration;
import :project.open_index;
import :server.query_commands;
import :vfs.file_system;

namespace clice::driver {

namespace {

struct RenameOptions {
    kota::deco::decl::HelpOption help;

    DecoInput(meta_var = "<LOCATOR> <NEW-NAME>",
              help =
                  "The symbol, as a locator: its whole name, an answer's symbol, or "
                  "file:line[:column]; and its new name",
              required = false,
              arg_num = 2)
    <std::vector<std::string>> inputs;

    DecoKV(help =
               "A file listing renames to make at once, `<locator> => <new name>` a line; - "
               "reads them from stdin",
           required = false)
    <std::string> spec;

    DecoFlag(names = {"--dry-run"},
             help = "Answer with the planned edits without writing them",
             required = false)
    dry_run;

    WorkspaceOptions where;
    FreshOption fresh;
    LogLevelOption log;
};

/// The renames a list asks for: `<locator> => <new name>` a line, blank
/// lines and lines starting with `//` aside.
std::expected<std::vector<query::RenameRequest>, std::string> parse_renames(llvm::StringRef text) {
    std::vector<query::RenameRequest> requests;
    llvm::SmallVector<llvm::StringRef> lines;
    text.split(lines, '\n');
    for(std::size_t i = 0; i < lines.size(); i += 1) {
        auto line = lines[i].trim();
        if(line.empty() || line.starts_with("//")) {
            continue;
        }
        auto number = static_cast<int>(i + 1);
        auto [locator, new_name] = line.rsplit("=>");
        locator = locator.trim();
        new_name = new_name.trim();
        if(!line.contains("=>") || locator.empty() || new_name.empty()) {
            return std::unexpected(
                std::format("line {}: expected `<locator> => <new name>`", number));
        }
        auto parsed = index::SymbolQuery::parse(locator);
        if(!parsed) {
            return std::unexpected(std::format("line {}: {}", number, parsed.error()));
        }
        requests.push_back({
            .text = locator.str(),
            .locator = std::move(*parsed),
            .new_name = new_name.str(),
            .line = number,
        });
    }
    if(requests.empty()) {
        return std::unexpected("the rename list names no rename");
    }
    return requests;
}

/// The renames a --spec list asks for.
std::expected<std::vector<query::RenameRequest>, std::string> read_renames(llvm::StringRef spec) {
    if(spec == "-") {
        auto input = llvm::MemoryBuffer::getSTDIN();
        if(!input) {
            return std::unexpected(
                std::format("cannot read the renames from stdin: {}", input.getError().message()));
        }
        return parse_renames((*input)->getBuffer());
    }
    auto text = vfs::read_text(spec);
    if(!text) {
        return std::unexpected(
            std::format("cannot read {}: {}", spec.str(), text.error().message()));
    }
    return parse_renames(*text);
}

/// The rename `<LOCATOR> <NEW-NAME>` asks for.
std::expected<std::vector<query::RenameRequest>, std::string>
    one_rename(llvm::ArrayRef<std::string> args) {
    auto& text = args[0];
    auto locator = index::SymbolQuery::parse(text);
    if(!locator) {
        return std::unexpected(locator.error());
    }
    std::vector<query::RenameRequest> requests;
    requests.push_back({.text = text, .locator = std::move(*locator), .new_name = args[1]});
    return requests;
}

/// Write the batch's edits, every file's new text computed before the
/// first is written: a file whose text moved on since it was indexed
/// stops the whole rename, never half of it. Files are rewritten in
/// place, keeping their permissions, owner and links, each only while its
/// status still is the one taken before it was read.
std::expected<void, std::string> apply(FileTable& files, const feature::BatchPlan& plan) {
    struct Rewrite {
        std::string path;
        vfs::Stamp stamp;
        std::string text;
    };

    std::vector<Rewrite> rewrites;
    llvm::DenseSet<Fid> seen;
    for(auto& batch_edit: plan.edits) {
        auto& site = batch_edit.edit.site;
        auto file = site.file;
        if(!seen.insert(file).second) {
            continue;
        }
        auto path = files.resolve(file).str();
        auto status = fs::status(path);
        std::string content;
        if(auto error = status ? fs::read_file(content, path) : status.error()) {
            return std::unexpected(std::format("cannot read {}: {}", site.path, error.message()));
        }
        auto text = vfs::without_bom(content);
        auto renamed = feature::apply_renames(text, plan, file);
        if(!renamed) {
            return std::unexpected(
                std::format("{} changed since it was indexed; run with --fresh", site.path));
        }
        rewrites.push_back({
            .path = std::move(path),
            .stamp = vfs::Stamp::of(*status),
            .text = content.substr(0, content.size() - text.size()) + *renamed,
        });
    }
    for(std::size_t i = 0; i < rewrites.size(); i += 1) {
        auto& rewrite = rewrites[i];
        auto status = fs::status(rewrite.path);
        std::string failure;
        if(!status || vfs::Stamp::of(*status) != rewrite.stamp) {
            failure = std::format("{} changed while the rename was being written", rewrite.path);
        } else if(auto error = fs::write_file(rewrite.path, rewrite.text)) {
            failure = std::format("cannot write {}: {}", rewrite.path, error.message());
        } else {
            continue;
        }
        if(i != 0) {
            std::vector<std::string> done;
            for(auto& written: llvm::ArrayRef(rewrites).take_front(i)) {
                done.push_back(written.path);
            }
            failure +=
                std::format("; the rename stopped after rewriting {}", llvm::join(done, ", "));
        }
        return std::unexpected(std::move(failure));
    }
    return {};
}

int run_rename(const RenameOptions& opts) {
    std::expected<std::vector<query::RenameRequest>, std::string> requests;
    if(opts.spec) {
        if(opts.inputs) {
            return usage_error(Errors::JSON,
                               "--spec lists the renames; <LOCATOR> <NEW-NAME> names one");
        }
        requests = read_renames(*opts.spec);
    } else if(!opts.inputs) {
        return usage_error(Errors::JSON, "rename takes <LOCATOR> <NEW-NAME>, or --spec");
    } else {
        requests = one_rename(*opts.inputs);
    }
    if(!requests) {
        return fail(std::move(requests.error()));
    }
    auto spelling = opts.where.root();
    auto config = Config::load_from_workspace(CanonicalPath(spelling));
    auto configuration = opts.where.configuration.tag();
    if(auto checked = check_requested_configuration(config, configuration); !checked) {
        return fail(std::move(checked.error()));
    }
    std::vector<std::string> failed;
    if(opts.fresh.fresh) {
        auto refreshed = refresh(spelling, config, configuration);
        if(!refreshed) {
            return fail(refreshed.error());
        }
        failed = std::move(*refreshed);
    }

    // The build tells a unit the index never reached from a file outside
    // the build, so the writer's load restores it.
    FileTable files;
    files.spell_root(spelling);
    Project project{files};
    CommandResolver commands{project};
    ContextChoices saved;
    EditorContext contexts{project, commands, saved};
    auto loaded =
        load_index(project, commands, std::move(config), configuration, /*with_build=*/true);
    if(!loaded) {
        return fail(
            "the index could not be opened (the log above says why); run `clice index` or pass --fresh");
    }
    saved.bytes = std::move(loaded->contexts);
    contexts.load();

    index::FreshnessGate gate(files, {.check_disk = true});
    index::IndexQuery index_query(project.index, files, &gate, nullptr);
    query::Context ctx{.project = project, .contexts = contexts, .query = index_query};

    auto stale = [&](llvm::ArrayRef<std::string> listed) {
        std::vector<std::string> all(listed.begin(), listed.end());
        for(auto file: gate.withheld()) {
            all.emplace_back(files.display(file));
        }
        all.insert(all.end(), ctx.unindexed.begin(), ctx.unindexed.end());
        all.insert(all.end(), failed.begin(), failed.end());
        ranges::sort(all);
        auto duplicates = ranges::unique(all);
        all.erase(duplicates.begin(), duplicates.end());
        return all;
    };

    auto planned = query::rename(ctx, *requests);
    if(project.index.damaged()) {
        return fail(damaged_index_error(project));
    }
    auto& result = planned.result;
    // A unit --fresh failed to index answers from an index that may be old.
    bool blocked = planned.blocked() || !failed.empty();
    if(!blocked && !opts.dry_run && !planned.plan.edits.empty()) {
        if(auto applied = apply(files, planned.plan); !applied) {
            result.conflicts.push_back(applied.error());
            blocked = true;
        } else {
            result.applied = true;
        }
    }
    auto withheld = stale(result.stale);
    print_json(Answer{.result = std::move(result), .stale = std::move(withheld)});
    return blocked ? 1 : 0;
}

CommandHandler build() {
    std::vector<Operation> operations;
    operations.push_back({
        .name = "rename",
        .description = "Rename symbols across the workspace",
        .run = operation<RenameOptions>(
            "clice refactor rename (<LOCATOR> <NEW-NAME> | --spec <FILE>) [OPTIONS]",
            "refactor",
            Errors::JSON,
            run_rename),
    });
    return with_operations("Rewrite the workspace's sources through the persisted index",
                           std::move(operations),
                           Errors::JSON);
}

[[maybe_unused]] const bool registered = register_command(
    Command::refactor,
    {.description = "Rewrite the workspace's sources through the persisted index", .build = build});

}  // namespace

}  // namespace clice::driver
