module clice;

import :semantic.decls;
import :semantic.template_resolver.visibility;

namespace clice::types {

void Visibility::look_from(clang::SourceLocation location) {
    auto place = unit.produced_at(location);
    file = place.file.isValid() && !unit.focus_file().contains(place.file) ? place.file
                                                                           : clang::FileID();
    offset = place.offset;
}

bool Visibility::sees(const clang::NamedDecl* decl) {
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

const Visibility::Reach& Visibility::reach(clang::FileID file) {
    if(auto found = reached.find(file); found != reached.end()) {
        return found->second;
    }
    Reach brought{.files = {file}};
    llvm::SmallVector<clang::FileID> pending{file};
    auto& directives = unit.directives();
    while(!pending.empty()) {
        auto current = pending.pop_back_val();
        auto directive = directives.find(current);
        if(directive == directives.end()) {
            continue;
        }
        for(auto& import: directive->second.imports) {
            add_imported(import.full_name, brought.modules);
        }
        for(auto& include: directive->second.includes) {
            if(include.fid.isValid() && brought.files.insert(include.fid).second) {
                pending.push_back(include.fid);
            }
        }
    }
    return reached.try_emplace(file, std::move(brought)).first->second;
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
    auto declared_in = [&](const llvm::DenseSet<clang::FileID>& files,
                           const llvm::StringSet<>& modules) {
        return ranges::any_of(declared, [&](const auto& entry) {
            auto [fid, module] = entry;
            return files.contains(fid) ||
                   (module && modules.contains(module->getTopLevelModuleName()));
        });
    };
    auto& forced = reach(unit.sema().getPreprocessor().getPredefinesFileID());
    if(declared_in(forced.files, forced.modules)) {
        return 0;
    }
    constexpr auto none = std::numeric_limits<std::uint32_t>::max();
    auto directive = unit.directives().find(file);
    if(directive == unit.directives().end()) {
        return none;
    }
    auto past = [&](clang::SourceLocation location) {
        return unit.produced_at(location).offset + 1;
    };
    std::uint32_t first = none;
    llvm::StringSet<> modules;
    for(auto& import: directive->second.imports) {
        add_imported(import.full_name, modules);
        if(declared_in({}, modules)) {
            first = past(import.location);
            break;
        }
    }
    for(auto& include: directive->second.includes) {
        if(past(include.location) >= first) {
            break;
        }
        if(include.fid.isValid()) {
            auto& brought = reach(include.fid);
            if(declared_in(brought.files, brought.modules)) {
                return past(include.location);
            }
        }
    }
    return first;
}

}  // namespace clice::types
