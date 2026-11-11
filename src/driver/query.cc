module clice;

import :command.file_kind;
import :driver.driver;
import :driver.run;
import :feature.counterparts;
import :feature.query;
import :index.project_index;
import :index.search.locator;
import :project.project;
import :syntax.dependency_graph;
import :vfs.path;

namespace clice::driver {

namespace {

struct FileInfo {
    std::string path;
    std::string kind;
    std::optional<std::string> module_name;
};

struct ProjectFilesResult {
    std::vector<FileInfo> files;
    int total = 0;
};

struct DepEntry {
    std::string path;
    int depth = 0;
};

struct FileDepsResult {
    std::string file;
    std::vector<DepEntry> includes;
    std::vector<DepEntry> includers;
};

struct ImpactAnalysisResult {
    std::vector<std::string> direct_dependents;
    std::vector<std::string> transitive_dependents;
    std::vector<std::string> affected_modules;
};

/// The files reachable from `root` along `adjacent`, breadth first and
/// each once: direct neighbours at depth 1, `max_depth` levels at most,
/// 0 meaning unbounded.
std::vector<DepEntry> collect_deps(Project& project,
                                   Fid root,
                                   int max_depth,
                                   llvm::function_ref<llvm::SmallVector<Fid>(Fid)> adjacent) {
    std::vector<DepEntry> entries;
    llvm::SmallVector<std::pair<Fid, int>> queue{
        {root, 0}
    };
    llvm::DenseSet<Fid> visited{root};
    for(std::size_t i = 0; i < queue.size(); i += 1) {
        auto [id, depth] = queue[i];
        if(max_depth > 0 && depth >= max_depth) {
            continue;
        }
        for(auto next: adjacent(id)) {
            if(!visited.insert(next).second) {
                continue;
            }
            queue.push_back({next, depth + 1});
            entries.push_back({.path = project.file_table.display(next), .depth = depth + 1});
        }
    }
    return entries;
}

/// Which of the build's files `clice query files` lists.
enum class FileFilter : std::uint8_t { All, Source, Header, Module };

/// The build's files `filter` lists.
ProjectFilesResult project_files(Project& project, FileFilter filter) {
    ProjectFilesResult result;
    llvm::DenseSet<Fid> seen;
    for(auto member: project.members()) {
        auto file_path = project.file_table.resolve(member);
        if(file_path.empty()) {
            continue;
        }
        seen.insert(member);
        auto module_name = project.dep_graph.module_of(member);
        auto [kind, name] = !module_name.empty()        ? std::pair(FileFilter::Module, "module")
                            : is_header_path(file_path) ? std::pair(FileFilter::Header, "header")
                                                        : std::pair(FileFilter::Source, "source");
        if(filter != FileFilter::All && filter != kind) {
            continue;
        }
        FileInfo info{.path = project.file_table.display(member), .kind = name};
        if(!module_name.empty()) {
            info.module_name = module_name.str();
        }
        result.files.push_back(std::move(info));
    }
    if(filter == FileFilter::All || filter == FileFilter::Header) {
        project.index.for_each_indexed_file([&](Fid path_id, std::uint64_t) {
            auto path = project.file_table.resolve(path_id);
            if(!seen.contains(path_id) && is_header_path(path)) {
                seen.insert(path_id);
                result.files.push_back(
                    {.path = project.file_table.display(path_id), .kind = "header"});
            }
        });
    }
    result.total = static_cast<int>(result.files.size());
    return result;
}

/// Which way `clice query file-deps` follows include edges.
enum class IncludeDirection : std::uint8_t { Includes, Includers, Both };

/// The files reachable from `path` along include edges in `direction`;
/// `depth` levels at most, 0 meaning unbounded.
std::expected<FileDepsResult, std::string>
    file_deps(Project& project, const Spelling& path, IncludeDirection direction, int depth) {
    if(depth < 0) {
        return std::unexpected("depth must not be negative");
    }
    if(!fs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    FileDepsResult result{.file = path.str()};
    auto file = project.file_table.find(path);
    if(!file) {
        return result;
    }
    result.file = project.file_table.display(*file);
    if(direction != IncludeDirection::Includers) {
        result.includes = collect_deps(project, *file, depth, [&](Fid id) {
            return project.dep_graph.get_all_includes(id);
        });
    }
    if(direction != IncludeDirection::Includes) {
        result.includers = collect_deps(project, *file, depth, [&](Fid id) {
            return llvm::SmallVector<Fid>(project.dep_graph.get_includers(id));
        });
    }
    return result;
}

/// What a change to `path` reaches: its direct includers, the sources
/// hosting it, and the modules among them.
std::expected<ImpactAnalysisResult, std::string> impact_analysis(Project& project,
                                                                 const Spelling& path) {
    if(!fs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    ImpactAnalysisResult result;
    auto file = project.file_table.find(path);
    if(!file) {
        return result;
    }
    llvm::DenseSet<Fid> seen{*file};
    for(auto dependent: llvm::concat<const Fid>(project.dep_graph.get_includers(*file),
                                                project.dep_graph.get_forcing_units(*file))) {
        if(seen.insert(dependent).second) {
            result.direct_dependents.push_back(project.file_table.display(dependent));
        }
    }
    auto readers = project.readers(*file);
    ranges::sort(readers, {}, [&](Fid reader) { return project.file_table.resolve(reader); });
    for(auto reader: readers) {
        if(seen.insert(reader).second) {
            result.transitive_dependents.push_back(project.file_table.display(reader));
        }
    }
    for(auto reader: readers) {
        auto module_name = project.dep_graph.module_of(reader);
        if(!module_name.empty()) {
            result.affected_modules.push_back(module_name.str());
        }
    }
    auto module_name = project.dep_graph.module_of(*file);
    if(!module_name.empty()) {
        result.affected_modules.push_back(module_name.str());
    }
    return result;
}

/// What every question takes.
struct Options {
    ProjectOptions where;
    FreshOption fresh;
};

KANNOTATE_TYPE(struct ProjectFiles, description = "The build's files");
struct ProjectFiles {
    KANNOTATE(&filter, description = "Which files to list")
    FileFilter filter = FileFilter::All;

    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct FileDeps,
               description = "The files a file includes and the files including it");
struct FileDeps {
    FileTarget target;

    KANNOTATE(&direction, description = "Which way to follow the include edges")
    IncludeDirection direction = IncludeDirection::Both;

    KANNOTATE(&depth, description = "Levels to follow, 0 for all")
    int depth = 1;

    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(
    struct ImpactAnalysis,
    description = "What a change to a file reaches: its includers and the modules among them");
struct ImpactAnalysis {
    FileTarget target;
    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Counterparts,
               description = "The files a file pairs with: a header's sources, a source's headers");
struct Counterparts {
    FileTarget target;
    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct SymbolSearch, description = "The symbols a locator matches, best first");
struct SymbolSearch {
    KANNOTATE(&locator,
              cli::positional = true,
              cli::meta_var = "<LOCATOR>",
              description = "The symbols to list, as a locator (default: every symbol)")
    std::optional<std::string> locator;

    KANNOTATE(&limit, description = "At most this many symbols")
    int limit = 100;

    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct ReadSymbol,
               description = "A symbol's definition as text, with its documentation");
struct ReadSymbol {
    SymbolTarget target;
    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct DocumentSymbols, description = "A file's outline");
struct DocumentSymbols {
    FileTarget target;
    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Definition,
               description = "Where a symbol is defined, with its documentation");
struct Definition {
    SymbolTarget target;
    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct References, description = "Every place naming a symbol");
struct References {
    SymbolTarget target;

    KANNOTATE(&include_declaration, description = "List the declarations and the definition too")
    bool include_declaration = false;

    Options query;
    LogLevelOption log;
};

/// Which sides of a call graph `clice query call-graph` walks.
enum class CallDirection : std::uint8_t { Callers, Callees, Both };

KANNOTATE_TYPE(struct CallGraph, description = "A function's callers and callees");
struct CallGraph {
    SymbolTarget target;

    KANNOTATE(&direction, description = "Which sides to walk")
    CallDirection direction = CallDirection::Both;

    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Implementation,
               description = "The classes deriving from a class, the overrides of a function");
struct Implementation {
    SymbolTarget target;
    Options query;
    LogLevelOption log;
};

/// Which sides of a type hierarchy `clice query type-hierarchy` walks.
enum class TypeDirection : std::uint8_t { Supertypes, Subtypes, Both };

KANNOTATE_TYPE(struct TypeHierarchy, description = "A type's supertypes and subtypes");
struct TypeHierarchy {
    SymbolTarget target;

    KANNOTATE(&direction, description = "Which sides to walk")
    TypeDirection direction = TypeDirection::Both;

    Options query;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Query, description = "Ask the persisted index about the project");
struct Query {
    KANNOTATE(&operation, presence = required, cli::meta_var = "<question>")
    std::variant<ProjectFiles,
                 FileDeps,
                 ImpactAnalysis,
                 Counterparts,
                 SymbolSearch,
                 ReadSymbol,
                 DocumentSymbols,
                 Definition,
                 References,
                 CallGraph,
                 Implementation,
                 TypeHierarchy>
        operation;
};

/// Answer a question against the opened index: `ask` asks it of the project
/// and the query context. The build questions (`with_build`) walk manifests
/// and contexts, which only the writer's load restores; the index
/// questions bind the tables in place and touch nothing else. The answer
/// lists as stale the files withheld on the way, the units a --fresh
/// refresh could not index, whose shards are as absent as a withheld
/// file's, and the files asked about that the index holds no shard for.
template <typename Ask>
int run_query(const Options& opts, bool with_build, Ask ask) {
    auto opened = open_project(opts.where,
                               opts.fresh,
                               with_build ? Project::OpenOptions::Load::Members
                                          : Project::OpenOptions::Load::Rules);
    if(!opened) {
        return fail(std::move(opened.error()));
    }
    auto& project = (*opened)->project;
    auto ctx = query_context(project);
    auto result = ask(project, ctx);
    if(project.index.damaged()) {
        return fail(project.damaged_index_error());
    }

    auto listed = std::move(ctx.unindexed);
    listed.append_range((*opened)->failed);
    auto stale = stale_files(project, std::move(listed));
    if(result) {
        print_json(Answer{.result = std::move(*result), .stale = std::move(stale)});
        return 0;
    }
    Failure failure{.error = std::move(result.error()), .stale = std::move(stale)};
    if(!ctx.candidates.empty()) {
        failure.candidates = std::move(ctx.candidates);
    }
    print_json(failure);
    return 1;
}

/// A question about the file `question` names, which `ask` takes after
/// the project and the query context.
template <typename Question, typename Ask>
int ask_file(const Question& question, bool with_build, Ask ask) {
    return run_query(question.query,
                     with_build,
                     [&](Project& project, feature::query::Context& ctx) {
                         return ask(project, ctx, Spelling(question.target.path, Spelling::cwd()));
                     });
}

/// A question about the symbol `question` names, which `ask` takes after
/// the query context.
template <typename Question, typename Ask>
int ask_symbol(const Question& question, Ask ask) {
    return run_query(
        question.query,
        /*with_build=*/false,
        [&](Project&, feature::query::Context& ctx) {
            return index::SymbolQuery::parse(question.target.locator).and_then([&](auto locator) {
                return ask(ctx, std::move(locator));
            });
        });
}

int run(ProjectFiles& question) {
    return run_query(question.query,
                     /*with_build=*/true,
                     [&](Project& project, feature::query::Context&) {
                         return std::expected<ProjectFilesResult, std::string>(
                             project_files(project, question.filter));
                     });
}

int run(FileDeps& question) {
    return ask_file(question,
                    /*with_build=*/true,
                    [&](Project& project, feature::query::Context&, const Spelling& path) {
                        return file_deps(project, path, question.direction, question.depth);
                    });
}

int run(ImpactAnalysis& question) {
    return ask_file(question,
                    /*with_build=*/true,
                    [](Project& project, feature::query::Context&, const Spelling& path) {
                        return impact_analysis(project, path);
                    });
}

int run(Counterparts& question) {
    return ask_file(
        question,
        /*with_build=*/true,
        [](Project& project, feature::query::Context& ctx, const Spelling& path) {
            return feature::counterparts(ctx, project.dep_graph, project.members(), path);
        });
}

int run(SymbolSearch& question) {
    auto text = question.locator.value_or("");
    auto limit = static_cast<std::size_t>(std::max(question.limit, 0));
    return run_query(question.query,
                     /*with_build=*/false,
                     [&](Project&, feature::query::Context& ctx) {
                         return feature::query::symbol_search(ctx, text, limit);
                     });
}

int run(ReadSymbol& question) {
    return ask_symbol(question, [](feature::query::Context& ctx, index::SymbolQuery locator) {
        return feature::query::read_symbol(ctx, std::move(locator));
    });
}

int run(DocumentSymbols& question) {
    return ask_file(question,
                    /*with_build=*/false,
                    [](Project& project, feature::query::Context& ctx, const Spelling& path) {
                        return feature::query::document_symbols(
                            ctx,
                            path,
                            [&](Fid file) -> const clang::LangOptions& {
                                return project.dialect(file, nullptr);
                            });
                    });
}

int run(Definition& question) {
    return ask_symbol(question, [](feature::query::Context& ctx, index::SymbolQuery locator) {
        return feature::query::definition(ctx, std::move(locator));
    });
}

int run(References& question) {
    return ask_symbol(question, [&](feature::query::Context& ctx, index::SymbolQuery locator) {
        return feature::query::references(ctx, std::move(locator), question.include_declaration);
    });
}

int run(CallGraph& question) {
    return ask_symbol(question, [&](feature::query::Context& ctx, index::SymbolQuery locator) {
        auto direction = question.direction;
        return feature::query::call_graph(ctx,
                                          std::move(locator),
                                          {.callers = direction != CallDirection::Callees,
                                           .callees = direction != CallDirection::Callers});
    });
}

int run(Implementation& question) {
    return ask_symbol(question, [](feature::query::Context& ctx, index::SymbolQuery locator) {
        return feature::query::implementation(ctx, std::move(locator));
    });
}

int run(TypeHierarchy& question) {
    return ask_symbol(question, [&](feature::query::Context& ctx, index::SymbolQuery locator) {
        auto direction = question.direction;
        return feature::query::type_hierarchy(ctx,
                                              std::move(locator),
                                              {.supertypes = direction != TypeDirection::Subtypes,
                                               .subtypes = direction != TypeDirection::Supertypes});
    });
}

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(dispatch<Query>);

}  // namespace

}  // namespace clice::driver
