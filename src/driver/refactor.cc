module;

#include "modules/prelude.h"

module clice;

import :config.config;
import :driver.driver;
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
              required = false)
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
    std::unique_ptr<llvm::MemoryBuffer> list;
    if(spec == "-") {
        auto input = llvm::MemoryBuffer::getSTDIN();
        if(!input) {
            return std::unexpected(
                std::format("cannot read the renames from stdin: {}", input.getError().message()));
        }
        list = std::move(*input);
    } else {
        auto file = vfs::read(spec);
        if(!file) {
            return std::unexpected(
                std::format("cannot read {}: {}", spec.str(), file.error().message()));
        }
        list = std::move(*file);
    }
    return parse_renames(list->getBuffer());
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
std::expected<void, std::string> apply(FileTable& files, const index::BatchPlan& plan) {
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
        auto status = vfs::status(path);
        auto bytes = vfs::read(path, vfs::Read::Bytes);
        if(!status || !bytes) {
            auto error = status ? bytes.error() : status.error();
            return std::unexpected(std::format("cannot read {}: {}", site.path, error.message()));
        }
        auto content = (*bytes)->getBuffer();
        auto text = vfs::without_bom(content);
        auto renamed = index::apply_renames(text, plan, file);
        if(!renamed) {
            return std::unexpected(
                std::format("{} changed since it was indexed; run with --fresh", site.path));
        }
        rewrites.push_back({
            .path = std::move(path),
            .stamp = status->stamp,
            .text = content.take_front(content.size() - text.size()).str() + *renamed,
        });
    }
    for(std::size_t i = 0; i < rewrites.size(); i += 1) {
        auto& rewrite = rewrites[i];
        auto status = vfs::status(rewrite.path);
        std::string failure;
        if(!status || status->stamp != rewrite.stamp) {
            failure = std::format("{} changed while the rename was being written", rewrite.path);
        } else if(auto error = vfs::write(rewrite.path, rewrite.text)) {
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

int run_rename(const RenameOptions& opts, const char* self_path) {
    std::expected<std::vector<query::RenameRequest>, std::string> requests;
    if(opts.spec) {
        if(opts.inputs) {
            return usage_error(Errors::JSON,
                               "--spec lists the renames; <LOCATOR> <NEW-NAME> names one");
        }
        requests = read_renames(*opts.spec);
    } else {
        auto args = positional(opts.inputs.as_optional(), {"<LOCATOR>", "<NEW-NAME>"});
        if(!args) {
            return usage_error(Errors::JSON, args.error());
        }
        requests = one_rename(*args);
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
        auto refreshed = refresh(spelling, config, configuration, self_path);
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
    ContextsBlob saved;
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
    index::IndexQuery index_query(project.project_index, files, &gate, nullptr);
    query::Context ctx{.project = project, .contexts = contexts, .query = index_query};

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

    auto planned = query::rename(ctx, *requests);
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

CommandHandler build(const char* self_path) {
    std::vector<Operation> operations;
    operations.push_back({
        .name = "rename",
        .description = "Rename symbols across the workspace",
        .run = operation<RenameOptions>(
            "clice refactor rename (<LOCATOR> <NEW-NAME> | --spec <FILE>) [OPTIONS]",
            "refactor",
            Errors::JSON,
            [self_path](const RenameOptions& opts) { return run_rename(opts, self_path); }),
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
