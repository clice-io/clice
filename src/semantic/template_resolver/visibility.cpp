module clice;

import :semantic.decls;
import :semantic.template_resolver.visibility;

namespace clice::types {

void Visibility::look_from(clang::SourceLocation location) {
    looked_from = location;
    placed = false;
}

void Visibility::place() {
    if(placed) {
        return;
    }
    placed = true;
    auto place = unit.produced_at(looked_from);
    file = place.file.isValid() && !unit.focus_file().contains(place.file) ? place.file
                                                                           : clang::FileID();
    offset = place.offset;
    unsigned passed = 0;
    if(file.isValid()) {
        auto bounds = boundaries(file);
        passed = ranges::upper_bound(bounds, offset) - bounds.begin();
    }
    current = views.try_emplace({file, passed}, views.size() + 1).first->second;
}

bool Visibility::sees(const clang::NamedDecl* decl) {
    place();
    if(file.isInvalid()) {
        return true;
    }
    if(auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(decl);
       shadow && !sees(shadow->getTargetDecl())) {
        return false;
    }
    auto [found, added] = visible_from.try_emplace({file, decl->getCanonicalDecl()}, 0);
    if(added) {
        found->second = first_visible(decl, file);
    }
    return found->second <= offset;
}

void Visibility::add_imported(llvm::StringRef name, llvm::StringSet<>& modules) {
    if(name.empty()) {
        return;
    }
    auto& map = unit.sema().getPreprocessor().getHeaderSearchInfo().getModuleMap();
    auto* module = map.findModule(name);
    if(!module) {
        modules.insert(name);
        return;
    }
    llvm::SmallVector<clang::Module*> pending{module};
    while(!pending.empty()) {
        auto* current = pending.pop_back_val();
        if(modules.insert(current->getTopLevelModuleName()).second) {
            current->getExportedModules(pending);
        }
    }
}

unsigned Visibility::node(clang::FileID file) {
    auto [found, added] = nodes.try_emplace(file, includes.size());
    if(added) {
        includes.emplace_back();
        imports.emplace_back();
    }
    return found->second;
}

std::optional<unsigned> Visibility::known_node(clang::FileID file) const {
    auto found = nodes.find(file);
    return found == nodes.end() ? std::nullopt : std::optional(found->second);
}

void Visibility::lay_out() {
    if(laid_out) {
        return;
    }
    laid_out = true;
    for(auto& [file, directive]: unit.directives()) {
        auto from = node(file);
        for(auto& import: directive.imports) {
            imports[from].push_back(import.full_name);
        }
        for(auto& include: directive.includes) {
            if(include.fid.isValid()) {
                auto to = node(include.fid);
                includes[from].push_back(to);
            }
        }
    }
}

const Visibility::Reach& Visibility::reach(clang::FileID file) {
    if(auto found = reached.find(file); found != reached.end()) {
        return found->second;
    }
    lay_out();
    auto start = node(file);
    Reach brought;
    brought.files.resize(includes.size());
    brought.files.set(start);
    llvm::SmallVector<unsigned> pending{start};
    while(!pending.empty()) {
        auto current = pending.pop_back_val();
        for(auto name: imports[current]) {
            add_imported(name, brought.modules);
        }
        for(auto to: includes[current]) {
            if(!brought.files.test(to)) {
                brought.files.set(to);
                pending.push_back(to);
            }
        }
    }
    return reached.try_emplace(file, std::move(brought)).first->second;
}

llvm::ArrayRef<std::uint32_t> Visibility::boundaries(clang::FileID file) {
    auto [found, added] = file_boundaries.try_emplace(file);
    auto& bounds = found->second;
    if(!added) {
        return bounds;
    }
    if(auto directive = unit.directives().find(file); directive != unit.directives().end()) {
        for(auto& import: directive->second.imports) {
            bounds.push_back(past(import.location));
        }
        for(auto& include: directive->second.includes) {
            bounds.push_back(past(include.location));
        }
    }
    ranges::sort(bounds);
    return bounds;
}

std::uint32_t Visibility::past(clang::SourceLocation location) {
    return unit.produced_at(location).offset + 1;
}

std::uint32_t Visibility::first_visible(const clang::NamedDecl* decl, clang::FileID file) {
    if(decls::fixed_std_utility(decl)) {
        return 0;
    }
    llvm::SmallVector<std::pair<clang::FileID, const clang::Module*>, 2> declared;
    for(const auto* redecl: decl->redecls()) {
        declared.emplace_back(unit.produced_at(redecl->getLocation()).file,
                              redecl->getOwningModule());
    }
    if(ranges::any_of(declared, [&](const auto& entry) { return entry.first == file; })) {
        return 0;
    }
    auto declared_in = [&](const Reach& reach) {
        return ranges::any_of(declared, [&](const auto& entry) {
            auto [fid, module] = entry;
            return reach.brings(known_node(fid)) ||
                   (module && reach.modules.contains(module->getTopLevelModuleName()));
        });
    };
    if(declared_in(reach(unit.sema().getPreprocessor().getPredefinesFileID()))) {
        return 0;
    }
    constexpr auto none = std::numeric_limits<std::uint32_t>::max();
    auto directive = unit.directives().find(file);
    if(directive == unit.directives().end()) {
        return none;
    }
    std::uint32_t first = none;
    Reach imported;
    for(auto& import: directive->second.imports) {
        add_imported(import.full_name, imported.modules);
        if(declared_in(imported)) {
            first = past(import.location);
            break;
        }
    }
    for(auto& include: directive->second.includes) {
        if(past(include.location) >= first) {
            break;
        }
        if(include.fid.isValid() && declared_in(reach(include.fid))) {
            return past(include.location);
        }
    }
    return first;
}

}  // namespace clice::types
