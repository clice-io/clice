module clice;

import :command.file_kind;
import :command.search_config;
import :feature.code_actions;
import :feature.code_completion;
import :feature.protocol;
import :index.query;
import :index.query.site;
import :semantic.symbol;
import :syntax.dependency_graph;
import :syntax.include_pragmas;
import :syntax.include_resolver;
import :syntax.lexer;
import :vfs.dir_cache;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice::feature {

namespace {

/// `path` between the delimiters of an angled or a quoted include.
std::string delimit(llvm::StringRef path, bool angled) {
    return angled ? std::format("<{}>", path) : std::format(R"("{}")", path);
}

/// `argument`, quotes or brackets included, as an IWYU pragma names the
/// header to include instead: with the delimiters `delimiters` asks for,
/// else as written.
std::string pragma_argument(llvm::StringRef argument, const IncludeDelimiters& delimiters) {
    bool delimited = (argument.starts_with('"') && argument.ends_with('"')) ||
                     (argument.starts_with('<') && argument.ends_with('>'));
    if(argument.size() <= 2 || !delimited) {
        return argument.str();
    }
    auto path = argument.drop_front().drop_back();
    auto angled = delimiters.angled_for(path);
    return angled ? delimit(path, *angled) : argument.str();
}

/// The header, quotes or brackets included, an `#include` for `candidate`
/// names in `file`: a standard name's from the standard library's own
/// table (the C library's for a C file, `c`), another's the header the
/// index says declares it (include_argument), or the one its IWYU pragmas
/// name instead (`pragmas_of`): a private header's mapping
/// (pragma_argument), or a header including it that exports it, included
/// the same way in turn.
/// None when only a library's internal header is known.
std::optional<std::string> include_for(const index::IndexQuery& query,
                                       const DependencyGraph& graph,
                                       const ScopeCandidate& candidate,
                                       bool c,
                                       Fid file,
                                       IncludeNames& names,
                                       const IncludeDelimiters& delimiters,
                                       llvm::function_ref<const IncludePragmas&(Fid)> pragmas_of) {
    auto& files = query.file_table();
    namespace stdlib = clang::tooling::stdlib;
    std::string qualifier;
    for(const auto& segment: candidate.scope) {
        qualifier += segment + "::";
    }
    if(auto symbol = stdlib::Symbol::named(qualifier,
                                           candidate.name,
                                           c ? stdlib::Lang::C : stdlib::Lang::CXX)) {
        if(auto header = symbol->header()) {
            return header->name().str();
        }
    }
    // The canonical declaration's header first, a definition when there is
    // one; then the headers declaring the symbol, for a definition in a
    // source file or in a header no `#include` can name.
    llvm::SmallVector<Fid, 2> headers;
    if(auto located = query.resolve(candidate.symbol, file);
       located && is_header_path(located->site.path)) {
        headers.push_back(located->site.file);
    }
    for(const auto& site: query.sites(candidate.symbol, file, RelationKind::Declaration)) {
        if(site.file.valid() && is_header_path(site.path) &&
           !ranges::contains(headers, site.file)) {
            headers.push_back(site.file);
        }
    }
    llvm::SmallVector<Fid, 4> visited;
    auto include_of = [&](auto& self, Fid header) -> std::optional<std::string> {
        auto path = files.resolve(header);
        if(header == file || is_context_header_path(path) || ranges::contains(visited, header)) {
            return std::nullopt;
        }
        visited.push_back(header);
        auto& pragmas = pragmas_of(header);
        if(!pragmas.include_instead.empty()) {
            return pragma_argument(pragmas.include_instead, delimiters);
        }
        if(!pragmas.is_private) {
            return include_argument(names, delimiters, path);
        }
        // An export names the private header as its includer wrote it:
        // beside the includer, or under a search directory.
        for(auto includer: graph.get_includers(header)) {
            auto& exporter = pragmas_of(includer);
            llvm::StringRef exporter_path = files.resolve(includer);
            bool exports = ranges::any_of(exporter.exports, [&](llvm::StringRef name) {
                llvm::StringRef spelled = path;
                auto beside = path::join(path::parent(exporter_path), name);
                path::normalize(beside);
                if(spelled == beside) {
                    return true;
                }
                return spelled.size() > name.size() && spelled.ends_with(name) &&
                       path::is_separator(spelled[spelled.size() - name.size() - 1]);
            });
            if(exports) {
                if(auto spelled = self(self, includer)) {
                    return spelled;
                }
            }
        }
        return std::nullopt;
    };
    for(auto header: headers) {
        if(auto spelled = include_of(include_of, header)) {
            return spelled;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<bool> IncludeDelimiters::angled_for(llvm::StringRef path) const {
    auto matches = [&](const kotatsu::GlobPattern& glob) {
        return glob.match(path);
    };
    if(ranges::any_of(quoted, matches)) {
        return false;
    }
    if(ranges::any_of(angled, matches)) {
        return true;
    }
    return std::nullopt;
}

std::optional<std::string> include_argument(IncludeNames& names,
                                            const IncludeDelimiters& delimiters,
                                            CanonicalRef header) {
    auto name = names.of(header);
    if(!name) {
        return std::nullopt;
    }
    using enum IncludeName::Found;
    // LIMITATION: quotes for a header -I finds name another file where one
    // of the same path sits next to the including file or under -iquote
    // (a src/config.h beside an include/config.h), which quotes find
    // first. Only a project with two headers of one name meets it.
    bool angled = name->found == Toolchain ||
                  delimiters.angled_for(name->path).value_or(name->found == System);
    return delimit(name->path, angled);
}

auto finish_completion(protocol::CompletionList list,
                       const IncludePlace& include,
                       llvm::ArrayRef<ScopeCandidate> candidates,
                       const index::IndexQuery& query,
                       const DependencyGraph& graph,
                       Fid file,
                       const SearchConfig& search,
                       const IncludeDelimiters& delimiters,
                       std::uint32_t limit) -> protocol::CompletionList {
    IncludeInsertion insertion{.opens_fragment = include.opens_fragment};
    vfs::Scope scope(query.file_table().dirs);
    IncludeNames names(search, query.file_table().spelling(file), scope);
    // Read from the disk, once a header.
    llvm::DenseMap<Fid, IncludePragmas> pragmas;
    auto pragmas_of = [&](Fid header) -> const IncludePragmas& {
        auto [it, inserted] = pragmas.try_emplace(header);
        if(inserted) {
            auto path = query.file_table().resolve(header);
            if(auto read = vfs::read_observed(path)) {
                it->second = include_pragmas(
                    read->text,
                    dialect_of(path, include.c ? std::optional(clang::Language::C) : std::nullopt));
            }
        }
        return it->second;
    };
    std::erase_if(list.items, [&](protocol::CompletionItem& item) {
        auto symbol = data_symbol(item.data);
        if(!symbol) {
            return false;
        }
        item.data.reset();
        auto candidate = ranges::find_if(candidates, [&](const ScopeCandidate& each) {
            return each.symbol == *symbol;
        });
        assert(candidate != candidates.end() && "the worker names a candidate it was given");
        auto header =
            include_for(query, graph, *candidate, include.c, file, names, delimiters, pragmas_of);
        if(!header) {
            return true;
        }
        protocol::TextEdit edit{
            .range = {include.position, include.position},
            .new_text = insertion.text(*header),
        };
        item.additional_text_edits = std::vector{std::move(edit)};
        return false;
    });
    auto limited = completion_list(std::move(list.items), limit);
    limited.is_incomplete = limited.is_incomplete || list.is_incomplete;
    return limited;
}

}  // namespace clice::feature
