module clice;

import :driver.driver;
import :feature.counterparts;
import :feature.query;
import :index.symbol_query;
import :project.project;
import :project.project_reader;
import :project.queries;

namespace clice::driver {

namespace {

/// What every question takes.
struct Options {
    ProjectOptions where;
    FreshOption fresh;
};

struct FileQuestion {
    kota::deco::decl::HelpOption help;
    FileTarget target;
    Options query;
    LogLevelOption log;
};

struct FileDepsQuestion {
    kota::deco::decl::HelpOption help;
    FileTarget target;

    DecoKV(help = "includes, includers or both (default)", required = false)
    <std::string> direction;

    DecoKV(help = "Levels to follow (default 1, 0 for all)", required = false)
    <int> depth;

    Options query;
    LogLevelOption log;
};

struct ProjectFilesQuestion {
    kota::deco::decl::HelpOption help;

    DecoKV(help = "all (default), source, header or module", required = false)
    <std::string> filter;

    Options query;
    LogLevelOption log;
};

struct SearchQuestion {
    kota::deco::decl::HelpOption help;

    DecoInput(meta_var = "<LOCATOR>",
              help = "The symbols to list, as a locator (default: every symbol)",
              required = false)
    <std::string> locator;

    DecoKV(help = "At most this many symbols (default 100)", required = false)
    <int> limit;

    Options query;
    LogLevelOption log;
};

struct SymbolQuestion {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;
    Options query;
    LogLevelOption log;
};

struct ReferencesQuestion {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;

    DecoFlag(names = {"--include-declaration"},
             help = "List the declarations and the definition too",
             required = false)
    include_declaration;

    Options query;
    LogLevelOption log;
};

struct CallGraphQuestion {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;

    DecoKV(help = "callers, callees or both (default)", required = false)
    <std::string> direction;

    Options query;
    LogLevelOption log;
};

struct TypeHierarchyQuestion {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;

    DecoKV(help = "supertypes, subtypes or both (default)", required = false)
    <std::string> direction;

    Options query;
    LogLevelOption log;
};

/// Answer a question against the opened index: `ask` asks it of the reader
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
                               with_build ? Project::OpenOptions::Build::Members
                                          : Project::OpenOptions::Build::None);
    if(!opened) {
        return fail(std::move(opened.error()));
    }
    auto& reader = *opened->reader;
    auto ctx = reader.context();
    auto outcome = ask(reader, ctx);
    if(reader.project.index.damaged()) {
        return fail(damaged_index_error(reader.project));
    }

    auto listed = std::move(ctx.unindexed);
    listed.append_range(opened->failed);
    auto stale = reader.stale(std::move(listed));
    if(outcome) {
        print_json(Answer{.result = std::move(*outcome), .stale = std::move(stale)});
        return 0;
    }
    Failure failure{.error = std::move(outcome.error()), .stale = std::move(stale)};
    if(!ctx.candidates.empty()) {
        failure.candidates = std::move(ctx.candidates);
    }
    print_json(failure);
    return 1;
}

/// A question about the file `ask` takes, after the reader and the query
/// context.
template <typename Question, typename Ask>
CommandHandler file_question(std::string_view usage, bool with_build, Ask ask) {
    return operation<Question>(usage, "query", Errors::JSON, [=](const Question& opts) {
        return run_query(
            opts.query,
            with_build,
            [&](ProjectReader& reader, feature::query::Context& ctx) {
                return ask(reader, ctx, Spelling(*opts.target.path, Spelling::cwd()), opts);
            });
    });
}

/// A question about the symbol `ask` takes, after the query context.
template <typename Question, typename Ask>
CommandHandler symbol_question(std::string_view usage, Ask ask) {
    return operation<Question>(usage, "query", Errors::JSON, [=](const Question& opts) {
        return run_query(
            opts.query,
            /*with_build=*/false,
            [&](ProjectReader&, feature::query::Context& ctx) {
                return index::SymbolQuery::parse(*opts.target.locator).and_then([&](auto locator) {
                    return ask(ctx, std::move(locator), opts);
                });
            });
    });
}

CommandHandler build() {
    std::vector<Operation> operations;
    operations.push_back({
        .name = "project-files",
        .description = "The build's files",
        .run = operation<ProjectFilesQuestion>(
            "clice query project-files [OPTIONS]",
            "query",
            Errors::JSON,
            [](const ProjectFilesQuestion& opts) {
                return run_query(opts.query,
                                 /*with_build=*/true,
                                 [&](ProjectReader& reader, feature::query::Context&) {
                                     return project_files(reader.project,
                                                          opts.filter.value_or("all"));
                                 });
            }),
    });
    operations.push_back({
        .name = "file-deps",
        .description = "The files a file includes and the files including it",
        .run = file_question<FileDepsQuestion>("clice query file-deps <PATH> [OPTIONS]",
                                               /*with_build=*/true,
                                               [](ProjectReader& reader,
                                                  feature::query::Context&,
                                                  const Spelling& path,
                                                  const FileDepsQuestion& opts) {
                                                   return file_deps(reader.project,
                                                                    path,
                                                                    opts.direction.value_or("both"),
                                                                    opts.depth.value_or(1));
                                               }),
    });
    operations.push_back({
        .name = "impact-analysis",
        .description = "What a change to a file reaches: its includers and the modules among them",
        .run = file_question<FileQuestion>(
            "clice query impact-analysis <PATH> [OPTIONS]",
            /*with_build=*/true,
            [](ProjectReader& reader,
               feature::query::Context&,
               const Spelling& path,
               const FileQuestion&) { return impact_analysis(reader.project, path); }),
    });
    operations.push_back({
        .name = "counterparts",
        .description = "The files a file pairs with: a header's sources, a source's headers",
        .run = file_question<FileQuestion>(
            "clice query counterparts <PATH> [OPTIONS]",
            /*with_build=*/true,
            [](ProjectReader& reader,
               feature::query::Context& ctx,
               const Spelling& path,
               const FileQuestion&) {
                auto& project = reader.project;
                return feature::counterparts(ctx, project.dep_graph, project.build.members(), path);
            }),
    });
    operations.push_back({
        .name = "symbol-search",
        .description = "The symbols a locator matches, best first",
        .run = operation<SearchQuestion>(
            "clice query symbol-search [<LOCATOR>] [OPTIONS]",
            "query",
            Errors::JSON,
            [](const SearchQuestion& opts) {
                auto text = opts.locator.value_or("");
                auto limit = static_cast<std::size_t>(std::max(opts.limit.value_or(100), 0));
                return run_query(opts.query,
                                 /*with_build=*/false,
                                 [&](ProjectReader&, feature::query::Context& ctx) {
                                     return feature::query::symbol_search(ctx, text, limit);
                                 });
            }),
    });
    operations.push_back({
        .name = "read-symbol",
        .description = "A symbol's definition as text, with its documentation",
        .run = symbol_question<SymbolQuestion>(
            "clice query read-symbol <LOCATOR> [OPTIONS]",
            [](feature::query::Context& ctx, index::SymbolQuery locator, const SymbolQuestion&) {
                return feature::query::read_symbol(ctx, std::move(locator));
            }),
    });
    operations.push_back({
        .name = "document-symbols",
        .description = "A file's outline",
        .run = file_question<FileQuestion>("clice query document-symbols <PATH> [OPTIONS]",
                                           /*with_build=*/false,
                                           [](ProjectReader& reader,
                                              feature::query::Context& ctx,
                                              const Spelling& path,
                                              const FileQuestion&) {
                                               return feature::query::document_symbols(
                                                   ctx,
                                                   path,
                                                   [&](Fid file) -> const clang::LangOptions& {
                                                       return reader.dialect(file);
                                                   });
                                           }),
    });
    operations.push_back({
        .name = "definition",
        .description = "Where a symbol is defined, with its documentation",
        .run = symbol_question<SymbolQuestion>(
            "clice query definition <LOCATOR> [OPTIONS]",
            [](feature::query::Context& ctx, index::SymbolQuery locator, const SymbolQuestion&) {
                return feature::query::definition(ctx, std::move(locator));
            }),
    });
    operations.push_back({
        .name = "references",
        .description = "Every place naming a symbol",
        .run = symbol_question<ReferencesQuestion>(
            "clice query references <LOCATOR> [OPTIONS]",
            [](feature::query::Context& ctx,
               index::SymbolQuery locator,
               const ReferencesQuestion& opts) {
                return feature::query::references(ctx,
                                                  std::move(locator),
                                                  static_cast<bool>(opts.include_declaration));
            }),
    });
    operations.push_back({
        .name = "call-graph",
        .description = "A function's callers and callees",
        .run = symbol_question<CallGraphQuestion>("clice query call-graph <LOCATOR> [OPTIONS]",
                                                  [](feature::query::Context& ctx,
                                                     index::SymbolQuery locator,
                                                     const CallGraphQuestion& opts) {
                                                      return feature::query::call_graph(
                                                          ctx,
                                                          std::move(locator),
                                                          opts.direction.value_or("both"));
                                                  }),
    });
    operations.push_back({
        .name = "implementation",
        .description = "The classes deriving from a class, the overrides of a function",
        .run = symbol_question<SymbolQuestion>(
            "clice query implementation <LOCATOR> [OPTIONS]",
            [](feature::query::Context& ctx, index::SymbolQuery locator, const SymbolQuestion&) {
                return feature::query::implementation(ctx, std::move(locator));
            }),
    });
    operations.push_back({
        .name = "type-hierarchy",
        .description = "A type's supertypes and subtypes",
        .run = symbol_question<TypeHierarchyQuestion>(
            "clice query type-hierarchy <LOCATOR> [OPTIONS]",
            [](feature::query::Context& ctx,
               index::SymbolQuery locator,
               const TypeHierarchyQuestion& opts) {
                return feature::query::type_hierarchy(ctx,
                                                      std::move(locator),
                                                      opts.direction.value_or("both"));
            }),
    });
    return with_operations("Ask the persisted index about the project",
                           std::move(operations),
                           Errors::JSON);
}

[[maybe_unused]] const bool registered =
    register_command(Command::query,
                     {.description = "Ask the persisted index about the project", .build = build});

}  // namespace

}  // namespace clice::driver
