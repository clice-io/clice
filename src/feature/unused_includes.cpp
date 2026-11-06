module clice;

import :compile.compilation;
import :feature.code_actions;
import :feature.diagnostics;
import :feature.unused_includes;
import :index.shard;
import :index.symbol;
import :index.tu_index;
import :semantic.decls;
import :semantic.semantics;
import :semantic.symbol;
import :semantic.template_resolver;
import :syntax.lexical_scan;
import :syntax.position;

namespace clice::feature {

namespace {

bool in_regions(llvm::ArrayRef<std::uint32_t> regions, std::uint32_t offset) {
    for(std::size_t i = 0; i + 1 < regions.size(); i += 2) {
        if(regions[i] <= offset && offset < regions[i + 1]) {
            return true;
        }
    }
    return false;
}

/// The regions `IWYU pragma: begin_keep` and `end_keep` comments enclose
/// (flat begin/end offset pairs), and the comments saying `IWYU pragma:
/// keep`.
struct KeepPragmas {
    std::vector<std::uint32_t> regions;

    std::vector<std::uint32_t> keeps;

    KeepPragmas(llvm::ArrayRef<LexicalInfo::Comment> comments, llvm::StringRef text) {
        constexpr llvm::StringLiteral pragma = "IWYU pragma:";
        std::optional<std::uint32_t> open;
        for(auto& comment: comments) {
            auto spelled = text.substr(comment.range.begin, comment.range.length());
            auto at = spelled.find(pragma);
            if(at == llvm::StringRef::npos) {
                continue;
            }
            auto word = spelled.drop_front(at + pragma.size()).ltrim();
            if(word.starts_with("begin_keep")) {
                open = open.value_or(comment.range.begin);
            } else if(word.starts_with("end_keep")) {
                if(open) {
                    regions.push_back(*open);
                    regions.push_back(comment.range.end);
                    open.reset();
                }
            } else if(word.starts_with("keep")) {
                keeps.push_back(comment.range.begin);
            }
        }
        if(open) {
            regions.push_back(*open);
            regions.push_back(static_cast<std::uint32_t>(text.size()));
        }
    }

    /// Whether they keep the directive at `range`, a keep comment on its
    /// line counting.
    bool keep(LocalSourceRange range) const {
        return in_regions(regions, range.begin) || ranges::any_of(keeps, [&](std::uint32_t at) {
                   return range.begin <= at && at <= range.end;
               });
    }
};

/// The focus file's `#include`s it can do without (unused_includes), over
/// the include graph of the TU index, its files by their node.
class Analysis {
public:
    Analysis(CompilationUnit& unit, const index::TUIndex& index) :
        unit(unit), sources(unit.context().getSourceManager()), focus(unit.focus_file()),
        index(index) {}

    std::vector<FileDiagnostic> run() {
        add_graph();
        add_directives();
        closures.resize(adjacency.size());
        // A header the compile did not enter itself is the context's: a
        // precompiled header the build declares, a module.
        context = entered;
        context.resize(adjacency.size());
        context.flip();
        for(auto root: roots) {
            context |= closure(root);
        }

        add_using_directives();
        add_references();
        add_macros();
        add_emitted();
        // What the file names stays included even where the command's
        // `-include` or the build's PCH brings it: those are the build's
        // choice, not the file's.
        for(auto& [providers, before]: found) {
            needs.push_back({
                .providers = {providers.begin(), providers.end()},
                .before = before
            });
        }
        add_header_needs();
        return report(keep());
    }

private:
    struct Directive {
        /// From the `#` to the end of its line.
        LocalSourceRange range;

        /// The header name as written, its delimiters included.
        llvm::StringRef name;

        std::uint32_t header = 0;

        /// Whether it may be reported: an `#include` (not an
        /// `#include_next` or `#import`) of a guarded header the include
        /// tree holds, which no `IWYU pragma: keep` keeps.
        bool judged = false;
    };

    /// What the file uses: any of `providers` gives it, an `#include`
    /// before `before` has to bring one.
    struct Need {
        llvm::SmallVector<std::uint32_t, 2> providers;

        std::uint32_t before = 0;
    };

    std::uint32_t node(llvm::StringRef path) {
        auto [it, inserted] = nodes.try_emplace(path, adjacency.size());
        if(inserted) {
            adjacency.emplace_back();
            entered.resize(adjacency.size());
        }
        return it->second;
    }

    /// The include tree's edges, those of the directives that skipped a
    /// header included; its roots are the focus file's `#include`s and the
    /// command's `-include`s.
    void add_graph() {
        std::vector<std::uint32_t> files(index.node_count());
        for(std::uint32_t i = 0; i < index.node_count(); i += 1) {
            files[i] = node(index.path(index.node(i).file));
        }
        for(std::uint32_t i = 0; i < index.node_count(); i += 1) {
            auto tree_node = index.node(i);
            if(!tree_node.skipped) {
                entered.set(files[i]);
            }
            if(tree_node.parent != index::no_node) {
                adjacency[files[tree_node.parent]].push_back(files[i]);
            } else {
                tops.push_back({tree_node.line, files[i]});
            }
        }
        for(std::uint32_t section = 0; section < index.section_count(); section += 1) {
            sections.push_back(node(index.path(index.section_path(section))));
        }
    }

    /// The focus file's `#include`s the compile entered or skipped.
    void add_directives() {
        auto& lexical = focus.lexical();
        auto text = focus.content();
        auto positions = focus.positions();
        auto& search = unit.sema().getPreprocessor().getHeaderSearchInfo();
        KeepPragmas pragmas(lexical.comments, text);
        auto it = unit.directives().find(focus.file());
        if(it == unit.directives().end()) {
            return;
        }
        for(auto& include: it->second.includes) {
            auto offset = focus.offset(include.location);
            auto* directive = offset ? lexical.directive_at(text, *offset) : nullptr;
            auto file =
                include.fid.isValid() ? sources.getFileEntryRefForID(include.fid) : std::nullopt;
            if(!directive || !file) {
                continue;
            }
            auto words = lexical.words(*directive);
            if(words.size() < 2 || !words[1].is_header_name()) {
                continue;
            }
            // A spliced directive's header name starts at its splice.
            auto name = words[1].text(text);
            name = name.drop_front(name.find_first_of("<\""));
            auto header = node(unit.file_path(*file));
            if(name.front() == '<') {
                angled.try_emplace(name, header);
            }
            auto line = static_cast<std::uint32_t>(
                positions.position(directive->range.begin, PositionEncoding::UTF8)->line + 1);
            directives.push_back({
                .range = directive->range,
                .name = name,
                .header = header,
                .judged = words[0].text(text) == "include" &&
                          search.isFileMultipleIncludeGuarded(*file) &&
                          !pragmas.keep(directive->range) &&
                          ranges::contains(tops, std::pair(line, header)),
            });
        }
        for(auto top: tops) {
            if(ranges::none_of(directives, [&](const Directive& directive) {
                   return directive.header == top.second;
               })) {
                roots.push_back(top.second);
            }
        }
    }

    /// The headers `file` brings, itself included, the edges of the
    /// headers it skips too.
    const llvm::BitVector& closure(std::uint32_t file) {
        auto& reached = closures[file];
        if(!reached) {
            reached.emplace(adjacency.size());
            llvm::SmallVector<std::uint32_t> pending = {file};
            reached->set(file);
            while(!pending.empty()) {
                auto next = pending.pop_back_val();
                for(auto header: adjacency[next]) {
                    if(!reached->test(header)) {
                        reached->set(header);
                        pending.push_back(header);
                    }
                }
            }
        }
        return *reached;
    }

    const llvm::BitVector& brought(std::uint32_t directive) {
        return closure(directives[directive].header);
    }

    static bool gives(const llvm::BitVector& files, const Need& need) {
        return ranges::any_of(need.providers, [&](std::uint32_t file) { return files.test(file); });
    }

    /// The header of the include tree a declaration or a macro at
    /// `location` is in; none for one the file has without an `#include`:
    /// its own, the compiler's, a module's. A header without a guard is
    /// read anew by each inclusion, an X-macro table declaring what the
    /// macros around it say: what one of its inclusions declares comes from
    /// the guarded header that included it.
    std::optional<std::uint32_t> header_of(clang::SourceLocation location) {
        if(location.isInvalid()) {
            return std::nullopt;
        }
        auto& search = unit.sema().getPreprocessor().getHeaderSearchInfo();
        auto fid = unit.produced_at(location).file;
        while(true) {
            auto entry = fid.isValid() ? sources.getFileEntryRefForID(fid) : std::nullopt;
            if(!entry || focus.contains(fid)) {
                return std::nullopt;
            }
            auto included = sources.getIncludeLoc(fid);
            if(search.isFileMultipleIncludeGuarded(*entry) || included.isInvalid()) {
                auto it = nodes.find(unit.file_path(*entry));
                return it == nodes.end() ? std::nullopt : std::optional(it->second);
            }
            fid = sources.getFileID(included);
        }
    }

    void commit(llvm::SmallVector<std::uint32_t, 4> providers, std::uint32_t before) {
        if(providers.empty()) {
            return;
        }
        ranges::sort(providers);
        providers.erase(std::unique(providers.begin(), providers.end()), providers.end());
        auto [it, inserted] =
            found.try_emplace(std::vector<std::uint32_t>(providers.begin(), providers.end()),
                              before);
        if(!inserted) {
            it->second = std::min(it->second, before);
        }
    }

    /// What the using-directives of the headers at file scope bring into
    /// sight: a name of a namespace they nominate needs them too.
    void add_using_directives() {
        for(auto* directive: unit.tu()->using_directives()) {
            if(auto header = header_of(directive->getLocation())) {
                nominated[directive->getNominatedNamespace()->getCanonicalDecl()].push_back(
                    *header);
            }
        }
    }

    /// What the focus file's declarations name, the declarations of what
    /// it declares, and what the bodies the compile instantiated name: a
    /// function the file's call instantiates may need what only the file
    /// brings (the `operator<` a sort calls). Those are finished by the end
    /// of the file, so an `#include` anywhere before it serves.
    void add_references() {
        std::vector<const clang::Decl*> written;
        for(auto* decl: unit.tu()->noload_decls()) {
            if(focus.contains(unit.produced_at(decl->getLocation()).file)) {
                written.push_back(decl);
            }
        }
        std::vector<const clang::Decl*> instantiated;
        for(auto* decl: unit.instantiations()) {
            if(!llvm::isa<clang::CXXRecordDecl>(decl)) {
                instantiated.push_back(decl);
            }
        }
        auto semantics = Semantics::build(unit,
                                          {
                                              .all_files = true,
                                              .instantiations = instantiated,
                                              .written = written,
                                          });
        auto end = static_cast<std::uint32_t>(focus.content().size());
        auto* resolver = &unit.get<types::TemplateResolver>();
        for(std::uint32_t i = 0; i < semantics.node_entries().size(); i += 1) {
            auto& entry = semantics.node(i);
            if(!entry.node.is_ast()) {
                continue;
            }
            if(entry.flags.in_instantiation) {
                for(auto& reference: resolve_references(semantics, i, resolver)) {
                    if(!reference.kind.isDeclOrDef()) {
                        need(reference.decl, end);
                    }
                }
                continue;
            }
            auto at = focus.offset(entry.node.source_range().getBegin()).value_or(end);
            for(auto& reference: resolve_references(semantics, i, resolver)) {
                auto before = at;
                if(reference.location.isValid()) {
                    before = focus.offset(reference.location).value_or(at);
                }
                if(reference.kind.isDeclOrDef()) {
                    declared(reference.decl, before);
                } else {
                    need(reference.decl, before);
                }
            }
            if(auto* shadow = found_through(entry.node)) {
                if(auto header = header_of(shadow->getLocation())) {
                    commit({*header}, at);
                }
            }
        }
    }

    /// The macros the focus file expands or tests.
    void add_macros() {
        for(auto& [fid, directive]: unit.directives()) {
            for(auto& macro: directive.macros) {
                if(macro.kind != MacroRef::Ref) {
                    continue;
                }
                auto at = unit.produced_at(macro.loc);
                if(!focus.contains(at.file)) {
                    continue;
                }
                if(auto header = header_of(macro.macro->getDefinitionLoc())) {
                    commit({*header}, at.offset);
                }
            }
        }
    }

    /// What the headers define for the file to emit: without them it
    /// compiles to another object, the tests a `.inc` pastes in gone.
    void add_emitted() {
        auto end = static_cast<std::uint32_t>(focus.content().size());
        auto& ast = unit.context();
        llvm::SmallVector<const clang::DeclContext*> scopes = {unit.tu()};
        while(!scopes.empty()) {
            for(auto* decl: scopes.pop_back_val()->noload_decls()) {
                if(llvm::isa<clang::NamespaceDecl, clang::LinkageSpecDecl, clang::ExportDecl>(
                       decl)) {
                    scopes.push_back(llvm::cast<clang::DeclContext>(decl));
                } else if(ast.DeclMustBeEmitted(decl)) {
                    if(auto header = header_of(decl->getLocation())) {
                        commit({*header}, end);
                    }
                }
            }
        }
    }

    /// The using-declaration a name of `node` was found through.
    const static clang::UsingShadowDecl* found_through(const SemanticNode& node) {
        const clang::NamedDecl* found = nullptr;
        if(auto* reference = node.get<clang::DeclRefExpr>()) {
            found = reference->getFoundDecl();
        } else if(auto* member = node.get<clang::MemberExpr>()) {
            found = member->getFoundDecl().getDecl();
        } else if(auto* type = node.get<clang::TypeLoc>()) {
            if(auto using_type = type->getAs<clang::UsingTypeLoc>()) {
                found = using_type.getDecl();
            } else if(auto specialization = type->getAs<clang::TemplateSpecializationTypeLoc>()) {
                found = specialization.getTypePtr()->getTemplateName().getAsUsingShadowDecl();
            }
        }
        return llvm::dyn_cast_if_present<clang::UsingShadowDecl>(found);
    }

    /// A definition the unit sees: what using a class, an inline function
    /// or a variable may need whole.
    const static clang::Decl* definition_of(const clang::Decl* decl) {
        if(auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
            return tag->getDefinition();
        }
        if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            return function->getDefinition();
        }
        if(auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
            return variable->getDefinition();
        }
        if(auto* templated = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
            if(auto* pattern = templated->getTemplatedDecl()) {
                return definition_of(pattern);
            }
        }
        return nullptr;
    }

    /// What naming `decl` needs: its definition when the unit sees one,
    /// else any declaration; the declarations around it it needs too — the
    /// class a member is in, the class a typedef names, the template an
    /// instantiation comes from — and the using-directive it is seen
    /// through. A name of the standard library is given by its headers
    /// too.
    void need(const clang::NamedDecl* decl, std::uint32_t before) {
        // Any header reopens a namespace: what is named in it is needed.
        if(llvm::isa<clang::NamespaceDecl>(decl)) {
            return;
        }
        auto [it, inserted] = needed.try_emplace(decl, before);
        if(!inserted) {
            if(it->second <= before) {
                return;
            }
            it->second = before;
        }

        if(auto* pattern = decls::instantiated_from(decl); pattern && pattern != decl) {
            need(pattern, before);
        }
        if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl->getDeclContext())) {
            need(record, before);
        }
        if(auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
            if(auto* tag = alias->getUnderlyingType()->getAsTagDecl()) {
                need(tag, before);
            }
        }
        for(auto* scope = decl->getDeclContext(); scope; scope = scope->getParent()) {
            auto* space = llvm::dyn_cast<clang::NamespaceDecl>(scope);
            if(auto it = space ? nominated.find(space->getCanonicalDecl()) : nominated.end();
               it != nominated.end()) {
                for(auto header: it->second) {
                    commit({header}, before);
                }
            }
        }

        llvm::SmallVector<std::uint32_t, 4> providers;
        if(auto* definition = definition_of(decl)) {
            auto header = header_of(definition->getLocation());
            if(!header) {
                return;
            }
            providers.push_back(*header);
        } else {
            for(auto* redecl: decl->redecls()) {
                auto header = header_of(redecl->getLocation());
                if(!header) {
                    return;
                }
                providers.push_back(*header);
            }
        }
        if(auto symbol = standard(decl)) {
            for(auto header: symbol->headers()) {
                if(auto it = angled.find(header.name()); it != angled.end()) {
                    providers.push_back(it->second);
                }
            }
        }
        commit(std::move(providers), before);
    }

    /// What declaring `decl` in the focus file needs: one of the headers
    /// declaring it before, which the declaration stays the same entity
    /// as.
    void declared(const clang::NamedDecl* decl, std::uint32_t before) {
        llvm::SmallVector<std::uint32_t, 4> providers;
        for(auto* redecl: decl->redecls()) {
            if(auto header = header_of(redecl->getLocation())) {
                providers.push_back(*header);
            }
        }
        commit(std::move(providers), before);
    }

    /// What the headers name but neither declare nor include a declaration
    /// of: the file needs it before the first `#include` bringing such a
    /// header without it.
    void add_header_needs() {
        llvm::DenseMap<index::SymbolHash, llvm::SmallVector<std::uint32_t, 2>> declarations;
        std::vector<std::pair<std::uint32_t, std::vector<index::SymbolHash>>> named;
        for(std::uint32_t section = 0; section < index.section_count(); section += 1) {
            auto file = sections[section];
            std::vector<index::SymbolHash> symbols;
            index.section_shard(section).for_each_relation(
                [&](index::SymbolHash symbol, const index::Relation& relation) {
                    if(RelationKind(relation.kind).isDeclOrDef()) {
                        auto& files = declarations[symbol];
                        if(files.empty() || files.back() != file) {
                            files.push_back(file);
                        }
                    } else if(relation.kind == RelationKind::Reference) {
                        symbols.push_back(symbol);
                    }
                    return true;
                });
            if(index.section_path(section) != index.focus_file()) {
                ranges::sort(symbols);
                symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
                named.emplace_back(file, std::move(symbols));
            }
        }

        for(auto& [file, symbols]: named) {
            llvm::SmallVector<std::uint32_t> reachers;
            for(std::uint32_t i = 0; i < directives.size(); i += 1) {
                if(brought(i).test(file)) {
                    reachers.push_back(i);
                }
            }
            if(reachers.empty()) {
                continue;
            }
            auto& own = closure(file);
            for(auto symbol: symbols) {
                auto it = declarations.find(symbol);
                if(it == declarations.end()) {
                    continue;
                }
                Need need{.providers = it->second};
                if(gives(own, need) || gives(context, need)) {
                    continue;
                }
                for(auto reacher: reachers) {
                    if(!gives(brought(reacher), need)) {
                        need.before = directives[reacher].range.begin;
                        needs.push_back(std::move(need));
                        break;
                    }
                }
            }
        }
    }

    /// Which `#include`s stay: those not judged, those directly giving
    /// something the file needs, then in source order those bringing a
    /// need none kept brings.
    std::vector<bool> keep() {
        std::vector<bool> kept(directives.size());
        std::vector<bool> met(needs.size());
        auto take = [&](std::uint32_t i) {
            kept[i] = true;
            auto& files = brought(i);
            for(auto [n, need]: views::enumerate(needs)) {
                if(directives[i].range.begin < need.before && gives(files, need)) {
                    met[n] = true;
                }
            }
        };
        for(std::uint32_t i = 0; i < directives.size(); i += 1) {
            auto& directive = directives[i];
            if(!directive.judged || ranges::any_of(needs, [&](const Need& need) {
                   return directive.range.begin < need.before &&
                          ranges::contains(need.providers, directive.header);
               })) {
                take(i);
            }
        }
        for(std::uint32_t i = 0; i < directives.size(); i += 1) {
            if(kept[i]) {
                continue;
            }
            auto& files = brought(i);
            for(auto [n, need]: views::enumerate(needs)) {
                if(!met[n] && directives[i].range.begin < need.before && gives(files, need)) {
                    take(i);
                    break;
                }
            }
        }
        return kept;
    }

    std::vector<FileDiagnostic> report(const std::vector<bool>& kept) {
        std::vector<FileDiagnostic> unused;
        for(auto [i, directive]: views::enumerate(directives)) {
            if(kept[i]) {
                continue;
            }
            auto& files = brought(i);
            bool brings =
                ranges::any_of(needs, [&](const Need& need) { return gives(files, need); });
            unused.push_back({
                .range = directive.range,
                .level = DiagnosticLevel::Warning,
                .source = DiagnosticSource::Clice,
                .code = "unused-include",
                .message =
                    brings ? std::format("Everything this file uses from {} and the headers it "
                                         "includes comes from its other includes too",
                                         directive.name)
                           : std::format("Nothing this file uses comes from {} or the headers it "
                                         "includes",
                                         directive.name),
            });
        }
        return unused;
    }

    CompilationUnit& unit;

    const clang::SourceManager& sources;

    FocusFile focus;

    const index::TUIndex& index;

    clang::tooling::stdlib::Recognizer standard;

    /// Each file's node, by path.
    llvm::StringMap<std::uint32_t> nodes;

    /// The headers each file's `#include`s name, those it skips included.
    std::vector<llvm::SmallVector<std::uint32_t, 4>> adjacency;

    /// The files the compile entered.
    llvm::BitVector entered;

    /// The tree's roots, by their line and file.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tops;

    /// The `-include`s of the command.
    llvm::SmallVector<std::uint32_t> roots;

    /// The file of each section of the TU index.
    std::vector<std::uint32_t> sections;

    /// What the file gets without any `#include` of its own.
    llvm::BitVector context;

    std::vector<std::optional<llvm::BitVector>> closures;

    /// In source order.
    std::vector<Directive> directives;

    /// The focus file's angled `#include`s by their header name, the
    /// standard library's headers among them.
    llvm::StringMap<std::uint32_t> angled;

    /// The headers whose using-directives nominate a namespace.
    llvm::DenseMap<const clang::NamespaceDecl*, llvm::SmallVector<std::uint32_t, 1>> nominated;

    /// Each declaration needed, with the earliest offset it is needed at.
    llvm::DenseMap<const clang::NamedDecl*, std::uint32_t> needed;

    /// The needs the file's own code makes, by their providers.
    std::map<std::vector<std::uint32_t>, std::uint32_t> found;

    std::vector<Need> needs;
};

}  // namespace

auto unused_includes(CompilationUnit& unit, const index::TUIndex& index)
    -> std::vector<FileDiagnostic> {
    // Code that does not compile names less than it means to.
    if(unit.lang_options().IsHeaderFile ||
       ranges::any_of(unit.diagnostics(), [](const Diagnostic& diagnostic) {
           return diagnostic.id.source == DiagnosticSource::Clang &&
                  diagnostic.id.level >= DiagnosticLevel::Error;
       })) {
        return {};
    }
    return Analysis(unit, index).run();
}

auto unused_include_fixes(llvm::ArrayRef<FileDiagnostic> diagnostics,
                          llvm::StringRef text,
                          LocalSourceRange selection) -> std::vector<ResolvedAction> {
    // The directive's lines, their line break included.
    auto lines = [&](LocalSourceRange range) -> TextReplacement {
        auto begin = text.rfind('\n', range.begin);
        auto end = text.find('\n', range.end);
        return {
            .range = {begin == llvm::StringRef::npos ? 0 : static_cast<std::uint32_t>(begin + 1),
                      end == llvm::StringRef::npos ? static_cast<std::uint32_t>(text.size())
                                                   : static_cast<std::uint32_t>(end + 1)},
        };
    };
    std::vector<ResolvedAction> fixes;
    std::vector<TextReplacement> all;
    for(auto& diagnostic: diagnostics) {
        if(diagnostic.source != DiagnosticSource::Clice || diagnostic.code != "unused-include") {
            continue;
        }
        auto& range = diagnostic.range;
        all.push_back(lines(range));
        if(selection.begin <= range.end && range.begin <= selection.end) {
            fixes.push_back({
                .title =
                    std::format("remove '{}'", text.substr(range.begin, range.length()).rtrim()),
                .kind = protocol::CodeActionKind::QuickFix,
                .edits = {all.back()},
            });
        }
    }
    if(!fixes.empty() && all.size() > 1) {
        fixes.push_back({
            .title = "remove all unused includes",
            .kind = protocol::CodeActionKind::QuickFix,
            .edits = std::move(all),
        });
    }
    return fixes;
}

}  // namespace clice::feature
