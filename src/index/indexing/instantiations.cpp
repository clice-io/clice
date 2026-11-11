module clice;

import :compile.directive;
import :index.indexing.builder;
import :index.indexing.tu_index;
import :index.shard.instantiations;
import :index.symbol;
import :semantic.decls;
import :semantic.identity;
import :semantic.semantics;
import :semantic.symbol;
import :semantic.types;

namespace clice::index {

const clang::NamedDecl* Builder::in_pattern(const clang::NamedDecl* decl) {
    const auto* function =
        llvm::dyn_cast_if_present<clang::FunctionDecl>(decl->getParentFunctionOrMethod());
    const auto* pattern = function ? function->getTemplateInstantiationPattern() : nullptr;
    if(!pattern || pattern == function) {
        return decl;
    }
    auto key = [](const clang::NamedDecl* named) {
        return std::pair(named->getLocation().getRawEncoding(),
                         static_cast<unsigned>(named->getKind()));
    };
    auto [it, fresh] = pattern_locals.try_emplace(pattern);
    if(fresh) {
        // A member template's body can be written outside its class,
        // in a redeclaration of the pattern.
        for(const auto* redecl: pattern->redecls()) {
            for(const auto* param: redecl->parameters()) {
                it->second.try_emplace(key(param), param);
            }
            for(const auto* local: redecl->decls()) {
                if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(local)) {
                    it->second.try_emplace(key(named), named);
                }
            }
        }
    }
    auto found = it->second.find(key(decl));
    if(found == it->second.end()) {
        return decl;
    }
    // A specialization takes its parameters from whichever declaration
    // lookup met (`fwd.h` redeclaring the template): the definition's
    // text names the definition's, that declaration's signature its own.
    const auto* definition = pattern->getDefinition();
    if(const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(found->second);
       param && definition && param->getDeclContext() != definition && expression_begin.isValid()) {
        auto at = unit.seen_location(expression_begin);
        auto range = definition->getSourceRange();
        if(!unit.before(at, unit.seen_location(range.getBegin())) &&
           !unit.before(unit.seen_location(range.getEnd()), at)) {
            return definition->getParamDecl(param->getFunctionScopeIndex());
        }
    }
    return found->second;
}

clang::FileID Builder::home_of(const clang::Decl* decl) {
    const clang::Decl* pattern = nullptr;
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if(const auto* found = function->getTemplateInstantiationPattern()) {
            const auto* definition = found->getDefinition();
            pattern = definition ? definition : found;
        }
    } else if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        if(const auto* found = var->getTemplateInstantiationPattern()) {
            const auto* definition = found->getDefinition();
            pattern = definition ? definition : found;
        }
    } else if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        pattern = record->getTemplateInstantiationPattern();
    }
    return unit.seen_at((pattern ? pattern : decl)->getLocation()).file;
}

bool Builder::resolves_in(clang::FileID fid) {
    return fid.isValid() && !unit.from_context(fid);
}

std::vector<const clang::Decl*> Builder::walked_instantiations() {
    std::vector<const clang::Decl*> walked;
    for(const auto* decl: unit.instantiations()) {
        if(llvm::isa<clang::CXXRecordDecl>(decl) ||
           (!standard_library && decls::in_standard_library(decl))) {
            continue;
        }
        auto home = home_of(decl);
        if(!resolves_in(home)) {
            continue;
        }
        auto key = unit.get<EntityTable>().entity(llvm::cast<clang::NamedDecl>(decl));
        if(ranges::binary_search(known_instantiations, known_at(unit.file_path(home), key))) {
            resolved[home].skipped.push_back(key);
            continue;
        }
        walked.push_back(decl);
    }
    return walked;
}

std::vector<const clang::Decl*> Builder::expanding_decls() {
    auto expansions = unit.focus_file().macro_invocations();
    if(expansions.empty()) {
        return {};
    }
    auto before = [&](clang::SourceLocation left, clang::SourceLocation right) {
        return unit.before(left, right);
    };
    llvm::DenseSet<const clang::Decl*> own(unit.top_level_decls().begin(),
                                           unit.top_level_decls().end());
    std::vector<const clang::Decl*> decls;
    for(const auto* decl: unit.context().getTranslationUnitDecl()->noload_decls()) {
        if(decl->isFromASTFile() || own.contains(decl)) {
            continue;
        }
        // The declarations clang makes for itself are written nowhere.
        auto range = unit.produced_bounds(decl->getSourceRange());
        if(range.isInvalid()) {
            continue;
        }
        auto first = ranges::lower_bound(expansions, range.getBegin(), before);
        if(first != expansions.end() && !before(range.getEnd(), *first)) {
            decls.push_back(decl);
        }
    }
    return decls;
}

Builder::WrittenKey Builder::written_key(SymbolHash symbol, const Relation& relation) {
    // A candidate already names what an instantiation's reference
    // would; a guess does not, and the instantiation confirming it is
    // recorded.
    RelationKind kind = relation.kind;
    auto kind_class =
        kind.is_one_of(RelationKind::Candidate) ? RelationKind::Reference : kind.value();
    return {symbol,
            kind_class,
            relation.range.begin,
            relation.range.end,
            kind.carries_extent() ? 0 : relation.target};
}

const llvm::DenseSet<Builder::WrittenKey>& Builder::written_in(clang::FileID fid) {
    auto [it, fresh] = written.try_emplace(fid);
    if(fresh) {
        if(auto found = file_indices.find(fid); found != file_indices.end()) {
            for(auto& [symbol, relations]: found->second.relations) {
                for(auto& relation: relations) {
                    it->second.insert(written_key(symbol, relation));
                }
            }
        }
    }
    return it->second;
}

std::vector<InstantiationRecord> Builder::records_of(clang::FileID home, const FileIndex& entries) {
    const auto& known = written_in(home);
    std::vector<InstantiationRecord> records;
    for(auto& [symbol, relations]: entries.relations) {
        for(auto& relation: relations) {
            RelationKind kind = relation.kind;
            InstantiationRecord record{.range = relation.range, .target = symbol};
            if(kind.is_one_of(RelationKind::Reference, RelationKind::Candidate)) {
                record.kind = InstantiationKind::Reference;
            } else if(kind.is_one_of(RelationKind::Deduced)) {
                record.kind = InstantiationKind::Deduced;
            } else if(kind.is_one_of(RelationKind::Implied)) {
                record.kind = InstantiationKind::Implied;
            } else if(kind.is_one_of(RelationKind::Callee)) {
                record.kind = InstantiationKind::Call;
                record.target = relation.target;
                record.from = symbol;
            } else if(kind.is_one_of(RelationKind::TypeDefinition) && relation.range.valid()) {
                record.kind = InstantiationKind::TypeDefinition;
                record.target = relation.target;
                record.from = symbol;
            } else {
                continue;
            }
            if(!known.contains(written_key(symbol, relation))) {
                records.push_back(record);
            }
        }
    }
    return records;
}

void Builder::add_walked(clang::FileID home,
                         const clang::NamedDecl* decl,
                         std::vector<InstantiationRecord> records) {
    auto& file = resolved[home].walked;
    FileInstantiations::Key key{
        .key = unit.get<EntityTable>().entity(decl),
        .pattern = ensure_symbol(decls::normalize(decl)),
    };
    if(!records.empty()) {
        key.resolution = static_cast<std::uint32_t>(file.resolutions.size());
        file.resolutions.push_back(std::move(records));
    }
    file.keys.push_back(key);
}

void Builder::project_instantiations(llvm::ArrayRef<const clang::Decl*> walked) {
    llvm::DenseMap<clang::FileID, FileIndex> entries;
    Semantics::build(
        unit,
        {.all_files = true,
         .visit = [&](const Semantics& path, std::uint32_t index) { project_node(path, index); },
         .instantiations = walked,
         .enter =
             [&](const clang::Decl* root) {
                 instantiation_entries = &entries;
                 instantiated_function = llvm::dyn_cast<clang::FunctionDecl>(root);
             },
         .leave =
             [&](const clang::Decl* root) {
                 instantiation_entries = nullptr;
                 instantiated_function = nullptr;
                 close_instantiation(llvm::cast<clang::NamedDecl>(root), entries);
                 entries.clear();
             },
         .written = llvm::ArrayRef<const clang::Decl*>()});
}

void Builder::close_instantiation(const clang::NamedDecl* decl,
                                  llvm::DenseMap<clang::FileID, FileIndex>& entries) {
    auto home = home_of(decl);
    std::vector<InstantiationRecord> records;
    if(auto it = entries.find(home); it != entries.end()) {
        records = records_of(home, it->second);
    }
    // A body that includes a fragment (`#include "Instruction.def"`
    // in a switch) resolves names in the fragment's file too, which
    // a skipped key could not stand for: such an instantiation keeps
    // no key, every unit walks it, and each file holds its records
    // as a resolution of its own.
    bool spilled = false;
    for(auto& [fid, index]: entries) {
        if(fid == home || !resolves_in(fid)) {
            continue;
        }
        if(auto fragment = records_of(fid, index); !fragment.empty()) {
            resolved[fid].walked.resolutions.push_back(std::move(fragment));
            spilled = true;
        }
    }
    if(!spilled) {
        add_walked(home, decl, std::move(records));
    } else if(!records.empty()) {
        resolved[home].walked.resolutions.push_back(std::move(records));
    }
}

void Builder::project_bases(const clang::CXXRecordDecl* record) {
    const auto* pattern = record->getTemplateInstantiationPattern();
    if(!pattern || !record->hasDefinition() ||
       (!standard_library && decls::in_standard_library(record)) ||
       ranges::none_of(pattern->bases(), [](const clang::CXXBaseSpecifier& base) {
           return base.getType()->isDependentType();
       })) {
        return;
    }
    auto home = home_of(record);
    if(!resolves_in(home)) {
        return;
    }
    const auto& known = written_in(home);
    auto derived = ensure_symbol(decls::normalize(record));
    std::vector<InstantiationRecord> records;
    for(const auto& base: record->bases()) {
        const auto* target = types::decl_of(base.getType().getCanonicalType());
        if(!target) {
            continue;
        }
        auto written = unit.written_range(base.getTypeSourceInfo()->getTypeLoc().getSourceRange());
        if(!written || written->file != home) {
            continue;
        }
        auto symbol = ensure_symbol(decls::normalize(target));
        Relation relation{.kind = RelationKind::Base, .target = symbol};
        if(known.contains(written_key(derived, relation))) {
            continue;
        }
        records.push_back({.range = written->range,
                           .kind = InstantiationKind::Base,
                           .target = symbol,
                           .from = derived});
    }
    if(!records.empty()) {
        resolved[home].walked.resolutions.push_back(std::move(records));
    }
}

void Builder::project_expansions() {
    auto& entities = unit.get<EntityTable>();

    // The standard library's own macros: a system header's, by a name
    // reserved for the implementation (`__glibcxx_assert`).
    auto library_macro = [&](clang::SourceLocation invocation, clang::SourceLocation body) {
        if(!unit.in_library(body)) {
            return false;
        }
        auto name = unit.token_spelling(invocation);
        return clang::isReservedInAllContexts(
            unit.context().Idents.get(name).isReserved(unit.lang_options()));
    };

    // By the expansion the body token came out of; an argument or a
    // pasted token is spelled in no body.
    llvm::MapVector<clang::FileID, std::pair<clang::FileID, std::vector<InstantiationRecord>>>
        expansions;
    auto add = [&](clang::SourceLocation location, SymbolHash target, InstantiationKind kind) {
        auto expansion = location;
        while(location.isMacroID()) {
            expansion = location;
            location = unit.expansion_level(location).spelling;
        }
        auto level = unit.expansion_level(expansion);
        if(level.argument || unit.concatenated(location) ||
           (!standard_library && library_macro(level.start, location))) {
            return;
        }
        auto [fid, offset] = unit.written_at(location);
        if(!resolves_in(fid)) {
            return;
        }
        auto length = unit.token_length(location);
        auto& [home, records] = expansions[unit.file_id(expansion)];
        home = fid;
        records.push_back({
            .range = {offset, offset + length},
            .kind = kind,
            .target = target,
            .expansion = true
        });
    };
    for(auto& directive:
        views::transform(unit.directives(), [](auto& entry) -> auto& { return entry.second; })) {
        for(auto& macro: directive.macros) {
            if(macro.kind == MacroRef::Ref && macro.loc.isMacroID()) {
                add(macro.loc, entities.entity(macro.macro), InstantiationKind::Reference);
            }
        }
    }
    // What a body declares is each expansion's own: its uses alone.
    for(auto& name: body_names) {
        std::optional<InstantiationKind> kind;
        if(name.kind.is_one_of(RelationKind::Reference,
                               RelationKind::Candidate,
                               RelationKind::Guess)) {
            kind = InstantiationKind::Reference;
        } else if(name.kind.is_one_of(RelationKind::Deduced)) {
            kind = InstantiationKind::Deduced;
        } else if(name.kind.is_one_of(RelationKind::Implied)) {
            kind = InstantiationKind::Implied;
        }
        if(kind) {
            add(name.location, ensure_symbol(decls::normalize(name.decl)), *kind);
        }
    }
    for(auto& [home, records]: views::values(expansions)) {
        resolved[home].walked.resolutions.push_back(std::move(records));
    }
}

void Builder::collect_body_names(const Semantics& path, std::uint32_t index) {
    see_from(path, index);
    for(auto& occurrence: resolve_occurrences(path, index, place_resolver)) {
        if(occurrence.location.isMacroID() &&
           unit.focus_file().written_offset(occurrence.location)) {
            body_names.push_back({occurrence.decl, occurrence.kind, occurrence.location});
        }
    }
}

}  // namespace clice::index
