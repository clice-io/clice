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
import :support.text;
import :syntax.include_pragmas;
import :syntax.lexical_scan;
import :syntax.position;

namespace clice::feature {

namespace {

/// The focus file's `#include`s it can do without (unused_includes), over
/// the include graph of the TU index, its files by their node.
class Builder {
public:
    Builder(CompilationUnit& unit, const index::TUIndex& index) :
        unit(unit), sources(unit.context().getSourceManager()), focus(unit.focus_file()),
        index(index) {}

    std::vector<FileDiagnostic> build() {
        add_graph();
        add_directives();
        closures.resize(adjacency.size());
        // A header the compile did not enter itself is the context's: a
        // precompiled header the build declares, a module.
        context = entered;
        context.resize(adjacency.size());
        context.flip();
        for(auto header: given) {
            context |= closure(header);
        }

        add_file_scope();
        add_references();
        add_macros();
        // What the file names stays included even where the command's
        // `-include` or the build's PCH brings it: those are the build's
        // choice, not the file's.
        for(auto& [providers, uses]: own_needs) {
            Need need{
                .providers = {providers.begin(), providers.end()},
                .before = uses.first
            };
            // A first use no `#include` before it serves is of a declaration
            // ahead of the definition: the first use needs the declaration,
            // the last may need the definition.
            if(ranges::none_of(views::iota(std::size_t{0}, directives.size()), [&](std::size_t i) {
                   return directives[i].range.begin < need.before && gives(brought(i), need);
               })) {
                need.before = uses.last;
                if(!uses.declarers.empty()) {
                    ranges::sort(uses.declarers);
                    uses.declarers.erase(ranges::unique(uses.declarers).begin(),
                                         uses.declarers.end());
                    needs.push_back({.providers = uses.declarers, .before = uses.first});
                }
            }
            needs.push_back(std::move(need));
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

        /// An `#include` (not an `#include_next` or `#import`) of a
        /// guarded header the include tree holds, which no `IWYU pragma:
        /// keep` keeps.
        bool reportable = false;
    };

    /// What the file uses: any of `providers` gives it, an `#include`
    /// before `before` has to bring one.
    struct Need {
        llvm::SmallVector<std::uint32_t, 2> providers;

        std::uint32_t before = 0;
    };

    /// Where the file's own code uses what a set of headers gives.
    struct Uses {
        std::uint32_t first = 0;

        std::uint32_t last = 0;

        /// The headers declaring a definition used, any of which serves a
        /// use needing only the declaration.
        llvm::SmallVector<std::uint32_t, 2> declarers;
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
    /// header included; its top level is the focus file's `#include`s and
    /// the command's `-include`s.
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
                top_level.push_back({tree_node.line, files[i]});
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
        auto pragmas = include_pragmas(text, unit.lang_options());
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
                .reportable = words[0].text(text) == "include" &&
                              search.isFileMultipleIncludeGuarded(*file) &&
                              !ranges::contains(pragmas.keeps, name.drop_front().drop_back()) &&
                              ranges::contains(top_level, std::pair(line, header)),
            });
        }
        for(auto [line, header]: top_level) {
            if(ranges::none_of(directives, [&](const Directive& directive) {
                   return directive.header == header;
               })) {
                given.push_back(header);
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
    /// the guarded header that included it. A header the tree does not
    /// hold, one a precompiled header includes, is in the header of the
    /// tree that included it.
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
                if(auto it = nodes.find(unit.file_path(*entry)); it != nodes.end()) {
                    return it->second;
                }
                if(included.isInvalid()) {
                    return std::nullopt;
                }
            }
            fid = sources.getFileID(included);
        }
    }

    void add_need(llvm::SmallVector<std::uint32_t, 4> providers,
                  std::uint32_t before,
                  llvm::ArrayRef<std::uint32_t> declarers = {}) {
        if(providers.empty()) {
            return;
        }
        ranges::sort(providers);
        providers.erase(ranges::unique(providers).begin(), providers.end());
        auto [it, inserted] =
            own_needs.try_emplace(std::vector<std::uint32_t>(providers.begin(), providers.end()),
                                  Uses{.first = before, .last = before});
        auto& uses = it->second;
        uses.first = std::min(uses.first, before);
        uses.last = std::max(uses.last, before);
        uses.declarers.append(declarers.begin(), declarers.end());
    }

    /// What the headers declare at namespace scope beyond the names the
    /// file uses: the using-directives bringing a namespace's names into
    /// sight of another, which a name of the namespace they nominate needs
    /// too, and the definitions the file has to emit, without which it
    /// compiles to another object (the tests a `.inc` pastes in gone).
    void add_file_scope() {
        auto end = static_cast<std::uint32_t>(focus.content().size());
        auto& ast = unit.context();
        llvm::SmallVector<const clang::DeclContext*> scopes = {unit.tu()};
        while(!scopes.empty()) {
            for(auto* decl: scopes.pop_back_val()->noload_decls()) {
                if(llvm::isa<clang::NamespaceDecl, clang::LinkageSpecDecl, clang::ExportDecl>(
                       decl)) {
                    scopes.push_back(llvm::cast<clang::DeclContext>(decl));
                } else if(auto* directive = llvm::dyn_cast<clang::UsingDirectiveDecl>(decl)) {
                    if(auto header = header_of(directive->getLocation())) {
                        nominated[directive->getNominatedNamespace()->getCanonicalDecl()].push_back(
                            *header);
                    }
                } else if(ast.DeclMustBeEmitted(decl)) {
                    if(auto header = header_of(decl->getLocation())) {
                        add_need({*header}, end);
                    }
                }
            }
        }
    }

    /// What the focus file's declarations name, the declarations of what
    /// it declares, and what the bodies the compile instantiated name: a
    /// function the file's call instantiates may need what only the file
    /// brings (the `operator<` a sort calls), and the classes its template
    /// arguments name whole (the `delete` of a `std::unique_ptr<T>` the
    /// file resets). Those are finished by the end of the file, so an
    /// `#include` anywhere before it serves.
    void add_references() {
        std::vector<const clang::Decl*> written;
        for(auto* decl: unit.tu()->noload_decls()) {
            if(focus.contains(unit.produced_at(decl->getLocation()).file)) {
                written.push_back(decl);
            }
        }
        // A class the headers instantiate would make every header
        // instantiating its own types needed: only the file's are walked.
        std::vector<const clang::Decl*> instantiated;
        for(auto* decl: unit.instantiations()) {
            if(!llvm::isa<clang::TagDecl>(decl) || in_focus(point_of_instantiation(decl))) {
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
                    add_need({*header}, at);
                }
            }
        }
        for(auto* decl: instantiated) {
            if(llvm::isa<clang::TagDecl>(decl) || !in_focus(point_of_instantiation(decl))) {
                continue;
            }
            auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
            if(auto* arguments = function ? function->getTemplateSpecializationArgs() : nullptr) {
                need_arguments(arguments->asArray(), end);
            }
            if(auto* variable = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
                need_arguments(variable->getTemplateArgs().asArray(), end);
            }
            for(auto* scope = decl->getDeclContext(); scope; scope = scope->getParent()) {
                if(auto* specialization =
                       llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(scope)) {
                    need_arguments(specialization->getTemplateArgs().asArray(), end);
                }
            }
        }
    }

    bool in_focus(clang::SourceLocation location) {
        return location.isValid() && focus.contains(unit.produced_at(location).file);
    }

    /// Where the compile instantiated `decl`; none for a member enum or a
    /// class local to an instantiated function.
    static clang::SourceLocation point_of_instantiation(const clang::Decl* decl) {
        if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            return function->getPointOfInstantiation();
        }
        if(auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
            return variable->getPointOfInstantiation();
        }
        if(auto* specialization = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
            return specialization->getPointOfInstantiation();
        }
        if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
            if(auto* member = record->getMemberSpecializationInfo()) {
                return member->getPointOfInstantiation();
            }
        }
        return {};
    }

    /// The classes `arguments` name, which an instantiated body may need
    /// whole.
    void need_arguments(llvm::ArrayRef<clang::TemplateArgument> arguments, std::uint32_t before) {
        for(auto& argument: arguments) {
            if(argument.getKind() == clang::TemplateArgument::Pack) {
                need_arguments(argument.pack_elements(), before);
            } else if(argument.getKind() == clang::TemplateArgument::Type) {
                need_whole(argument.getAsType(), before);
            }
        }
    }

    /// A class `type` is, by value: what holds or passes it needs it whole.
    void need_whole(clang::QualType type, std::uint32_t before) {
        if(auto* tag = type->getAsTagDecl()) {
            need(tag, before);
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
                    add_need({*header}, at.offset);
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
    /// instantiation comes from, the classes a function takes and returns
    /// by value, the declarations giving it its default arguments — and
    /// the using-directive it is seen through. A name of the standard
    /// library is given by its headers too.
    void need(const clang::NamedDecl* decl, std::uint32_t before) {
        // Any header reopens a namespace: what is named in it is needed.
        if(llvm::isa<clang::NamespaceDecl>(decl)) {
            return;
        }
        auto [it, inserted] = needed.try_emplace(decl, before, before);
        if(!inserted) {
            auto& [first, last] = it->second;
            if(first <= before && before <= last) {
                return;
            }
            first = std::min(first, before);
            last = std::max(last, before);
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
                    add_need({header}, before);
                }
            }
        }
        if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            need_whole(function->getReturnType(), before);
            for(auto* parameter: function->parameters()) {
                need_whole(parameter->getType(), before);
            }
            for(auto* redecl: function->redecls()) {
                auto defaults = ranges::any_of(redecl->parameters(), [](const auto* parameter) {
                    return parameter->hasDefaultArg() && !parameter->hasInheritedDefaultArg();
                });
                if(auto header = defaults ? header_of(redecl->getLocation()) : std::nullopt) {
                    add_need({*header}, before);
                }
            }
        }

        llvm::SmallVector<std::uint32_t, 4> providers;
        llvm::SmallVector<std::uint32_t, 2> declarers;
        if(auto* definition = definition_of(decl)) {
            auto header = header_of(definition->getLocation());
            if(!header) {
                return;
            }
            providers.push_back(*header);
            for(auto* redecl: decl->redecls()) {
                if(auto declarer = header_of(redecl->getLocation())) {
                    declarers.push_back(*declarer);
                }
            }
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
        add_need(std::move(providers), before, declarers);
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
        add_need(std::move(providers), before);
    }

    /// What the headers name but neither define nor include a definition
    /// of, or a declaration of what the unit sees no definition of, and the
    /// macros they expand or test but neither define nor include: the file
    /// needs it before the first `#include` bringing such a header without
    /// it. A header naming a class it declares ahead may need it whole, as
    /// an inline function taking its `sizeof` does.
    void add_header_needs() {
        llvm::DenseMap<index::SymbolHash, llvm::SmallVector<std::uint32_t, 2>> declarations;
        llvm::DenseMap<index::SymbolHash, llvm::SmallVector<std::uint32_t, 2>> definitions;
        std::vector<std::pair<std::uint32_t, std::vector<index::SymbolHash>>> named;
        for(std::uint32_t section = 0; section < index.section_count(); section += 1) {
            auto file = sections[section];
            std::vector<index::SymbolHash> symbols;
            index.section_shard(section).for_each_relation([&](index::SymbolHash symbol,
                                                               const index::Relation& relation) {
                if(RelationKind(relation.kind).isDeclOrDef()) {
                    auto& files = relation.kind == RelationKind::Definition ? definitions[symbol]
                                                                            : declarations[symbol];
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
                symbols.erase(ranges::unique(symbols).begin(), symbols.end());
                named.emplace_back(file, std::move(symbols));
            }
        }

        // What each header uses, by the headers giving it.
        std::map<std::uint32_t, std::vector<llvm::SmallVector<std::uint32_t, 2>>> uses;
        for(auto& [file, symbols]: named) {
            for(auto symbol: symbols) {
                auto it = definitions.find(symbol);
                if(it == definitions.end()) {
                    it = declarations.find(symbol);
                    if(it == declarations.end()) {
                        continue;
                    }
                }
                uses[file].push_back(it->second);
            }
        }
        for(auto& [fid, directive]: unit.directives()) {
            for(auto& macro: directive.macros) {
                if(macro.kind != MacroRef::Ref) {
                    continue;
                }
                auto file = header_of(macro.loc);
                auto definition = file ? header_of(macro.macro->getDefinitionLoc()) : std::nullopt;
                if(definition && *definition != *file) {
                    uses[*file].push_back({*definition});
                }
            }
        }

        for(auto& [file, providers]: uses) {
            llvm::SmallVector<std::uint32_t> bringing;
            for(std::uint32_t i = 0; i < directives.size(); i += 1) {
                if(brought(i).test(file)) {
                    bringing.push_back(i);
                }
            }
            if(bringing.empty()) {
                continue;
            }
            auto& own = closure(file);
            for(auto& provider: providers) {
                Need need{.providers = provider};
                if(gives(own, need) || gives(context, need)) {
                    continue;
                }
                for(auto directive: bringing) {
                    if(!gives(brought(directive), need)) {
                        need.before = directives[directive].range.begin;
                        needs.push_back(std::move(need));
                        break;
                    }
                }
            }
        }
    }

    /// Which `#include`s stay: those not reportable, those directly giving
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
            if(!directive.reportable || ranges::any_of(needs, [&](const Need& need) {
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

private:
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

    /// The headers the focus file and the command include, by the line
    /// including them and their file.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> top_level;

    /// The headers the file gets without an `#include` naming them: the
    /// command's `-include`s, those an `#include` names through a macro.
    llvm::SmallVector<std::uint32_t> given;

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

    /// Each declaration needed, with the first and the last offset it is
    /// needed at.
    llvm::DenseMap<const clang::NamedDecl*, std::pair<std::uint32_t, std::uint32_t>> needed;

    /// The needs the file's own code makes, by their providers.
    std::map<std::vector<std::uint32_t>, Uses> own_needs;

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
    return Builder(unit, index).build();
}

auto unused_include_fixes(llvm::ArrayRef<FileDiagnostic> diagnostics,
                          llvm::StringRef text,
                          LocalSourceRange selection) -> std::vector<ResolvedAction> {
    auto lines = [&](LocalSourceRange range) -> TextReplacement {
        if(begins_line(text, range.begin)) {
            return {
                .range = {line_begin(text, range.begin), line_end(text, range.end)}
            };
        }
        // The end of a comment before the directive stays, on its line.
        auto begin = static_cast<std::uint32_t>(text.take_front(range.begin).rtrim(" \t").size());
        return {
            .range = {begin, range.end}
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
