module clice;

import :driver.driver;
import :driver.run;
import :feature.query;
import :feature.rename;
import :index.search.locator;
import :project.project;
import :vfs.file_system;

namespace clice::driver {

namespace {

/// One rename, given on the command line.
KANNOTATE_TYPE(struct OneRename, description = "one symbol");
struct OneRename {
    KANNOTATE(&locator,
              cli::positional = true,
              cli::meta_var = "<LOCATOR>",
              description =
                  "The symbol, as a locator: its whole name, an answer's symbol, or "
                  "file:line[:column]")
    std::string locator;

    KANNOTATE(&new_name,
              cli::positional = true,
              cli::meta_var = "<NEW-NAME>",
              description = "Its new name")
    std::string new_name;
};

/// The renames a file lists.
KANNOTATE_TYPE(struct RenameList, description = "a list of symbols");
struct RenameList {
    KANNOTATE(&spec,
              presence = required,
              cli::meta_var = "<FILE>",
              description =
                  "A file listing renames to make at once, `<locator> => <new name>` "
                  "a line; - reads them from stdin")
    std::string spec;
};

KANNOTATE_TYPE(struct Rename, description = "Rename symbols across the project");
struct Rename {
    KANNOTATE(&renames, cli::exclusive = true, presence = required, description = "Rename")
    std::variant<OneRename, RenameList> renames;

    KANNOTATE(&dry_run, description = "Answer with the planned edits without writing them")
    bool dry_run = false;

    ProjectOptions where;
    FreshOption fresh;
};

/// The renames a list asks for: `<locator> => <new name>` a line, blank
/// lines and lines starting with `//` aside.
std::expected<std::vector<feature::query::RenameRequest>, std::string>
    parse_renames(llvm::StringRef text) {
    std::vector<feature::query::RenameRequest> requests;
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
        auto parsed = index::Locator::parse(locator);
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
std::expected<std::vector<feature::query::RenameRequest>, std::string>
    read_renames(llvm::StringRef spec) {
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
        return std::unexpected(std::format("{}", text.error()));
    }
    return parse_renames(*text);
}

/// The rename `<LOCATOR> <NEW-NAME>` asks for.
std::expected<std::vector<feature::query::RenameRequest>, std::string>
    one_rename(const OneRename& rename) {
    auto locator = index::Locator::parse(rename.locator);
    if(!locator) {
        return std::unexpected(locator.error());
    }
    std::vector<feature::query::RenameRequest> requests;
    requests.push_back(
        {.text = rename.locator, .locator = std::move(*locator), .new_name = rename.new_name});
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
        } else if(auto written = vfs::write_file(rewrite.path, rewrite.text); !written) {
            failure = std::format("{}", written.error());
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

int run(Rename& opts) {
    auto* one = std::get_if<OneRename>(&opts.renames);
    auto requests = one ? one_rename(*one) : read_renames(std::get<RenameList>(opts.renames).spec);
    if(!requests) {
        return fail(std::move(requests.error()));
    }
    // The build tells a unit the index never reached from a file outside
    // the build, so the writer's load restores it.
    auto opened = open_project(opts.where, opts.fresh, Project::OpenOptions::Load::Members);
    if(!opened) {
        return fail(std::move(opened.error()));
    }
    auto& project = (*opened)->project;
    auto& failed = (*opened)->failed;
    auto ctx = query_context(project);
    auto editable_files = project.editable_files();
    auto editable = [&](llvm::StringRef path) {
        return editable_files.editable(path);
    };
    auto read = [](llvm::StringRef path) -> std::optional<std::string> {
        auto text = vfs::read_text(path);
        return text ? std::optional(std::move(*text)) : std::nullopt;
    };
    auto planned = feature::query::rename(
        ctx,
        *requests,
        {.editable = editable, .read = read, .units_pending = project.units_pending()});
    if(project.index.damaged()) {
        return fail(ProjectError{.message = project.damaged_index_error()});
    }
    auto& result = planned.result;
    // A unit --fresh failed to index answers from an index that may be old.
    bool blocked = planned.blocked() || !failed.empty();
    if(!blocked && !opts.dry_run && !planned.plan.edits.empty()) {
        if(auto applied = apply(project.file_table, planned.plan); !applied) {
            result.conflicts.push_back(applied.error());
            blocked = true;
        } else {
            result.applied = true;
        }
    }
    auto stale = stale_files(project, result.stale, std::move(ctx.unindexed), failed);
    if(auto note = stale.note(); !note.empty()) {
        for(auto& rename: result.renames) {
            if(rename.error) {
                rename.error = std::format("{}; {}", *rename.error, note);
            }
        }
    }
    print_json(Answer{.result = std::move(result), .stale = stale.all()});
    return blocked ? 1 : 0;
}

KANNOTATE_TYPE(struct Refactor,
               description = "Rewrite the project's sources through the persisted index");
struct Refactor {
    KANNOTATE(&operation, presence = required, cli::meta_var = "<refactoring>")
    std::variant<Rename> operation;
};

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(dispatch<Refactor>);

}  // namespace

}  // namespace clice::driver
