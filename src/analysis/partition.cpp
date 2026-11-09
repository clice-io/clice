module clice;

import :analysis.partition;
import :syntax.lexer;
import :vfs.path;

namespace clice::analysis {

bool is_keyword(llvm::StringRef word) {
    return keyword_kind(
               word,
               keywords(raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx23))) !=
           clang::tok::identifier;
}

bool is_module_name(llvm::StringRef name) {
    llvm::SmallVector<llvm::StringRef> parts;
    name.split(parts, '.');
    return ranges::all_of(parts, [](llvm::StringRef part) {
        return !part.empty() && (llvm::isAlpha(part.front()) || part.front() == '_') &&
               ranges::all_of(part, [](char c) { return llvm::isAlnum(c) || c == '_'; }) &&
               !is_keyword(part) && part != "module" && part != "import";
    });
}

std::expected<Partition, std::string> partition(const Facts& facts, const PartitionSpec& spec) {
    Partition result;
    llvm::StringMap<std::uint32_t> ids;
    auto module_id = [&](llvm::StringRef name) {
        auto [it, inserted] =
            ids.try_emplace(name, static_cast<std::uint32_t>(result.modules.size()));
        if(inserted) {
            result.modules.push_back(name.str());
        }
        return it->second;
    };

    std::vector<std::pair<llvm::StringRef, kotatsu::GlobPattern>> globs;
    for(auto& module: spec.modules) {
        for(auto& pattern: module.files) {
            auto glob = kotatsu::GlobPattern::create(pattern);
            if(!glob) {
                return std::unexpected(
                    std::format("invalid glob '{}': {}", pattern, glob.error().message));
            }
            globs.emplace_back(module.name, std::move(*glob));
        }
    }

    auto directory = [&](llvm::StringRef path) {
        llvm::StringRef dir(path::parent(path, path::Style::Posix));
        if(dir.empty()) {
            return std::string(".");
        }
        if(spec.depth == 0) {
            return dir.str();
        }
        llvm::SmallVector<llvm::StringRef> segments;
        dir.split(segments, '/');
        segments.truncate(std::min<std::size_t>(spec.depth, segments.size()));
        return llvm::join(segments, "/");
    };

    // The names whose glob matches a file; a directory module that happens to
    // share one stays the program's.
    llvm::StringSet<> matched_names;
    for(auto& file: facts.files) {
        auto matched =
            ranges::find_if(globs, [&](auto& glob) { return glob.second.match(file.path); });
        if(matched != globs.end()) {
            matched_names.insert(matched->first);
        }
        result.module_of.push_back(module_id(
            matched != globs.end() ? matched->first : llvm::StringRef(directory(file.path))));
    }

    for(llvm::StringRef move: spec.moves) {
        auto [path, module] = move.rsplit('=');
        auto it = facts.file_ids.find(path);
        if(module.empty() || it == facts.file_ids.end()) {
            return std::unexpected(std::format("--move {}: no scoped file {}",
                                               std::string_view(move),
                                               std::string_view(path)));
        }
        result.module_of[it->second] = module_id(module);
    }

    // A module merged away answers to the one it joined.
    std::vector<std::uint32_t> joined(result.modules.size());
    for(std::uint32_t module = 0; module < joined.size(); module += 1) {
        joined[module] = module;
    }
    for(llvm::StringRef merge: spec.merges) {
        llvm::SmallVector<llvm::StringRef> names;
        merge.split(names, '+');
        llvm::SmallVector<std::uint32_t> members;
        for(auto name: names) {
            auto it = ids.find(name);
            if(it == ids.end()) {
                return std::unexpected(std::format("--merge {}: no module {}",
                                                   std::string_view(merge),
                                                   std::string_view(name)));
            }
            auto module = it->second;
            while(joined[module] != module) {
                module = joined[module];
            }
            members.push_back(module);
        }
        for(auto member: members) {
            joined[member] = members.front();
        }
        for(auto& module: result.module_of) {
            if(ranges::contains(members, module)) {
                module = members.front();
            }
        }
    }

    // Modules a move or merge emptied leave the table.
    std::vector<std::uint32_t> remap(result.modules.size(), none);
    std::vector<std::string> modules;
    for(auto& module: result.module_of) {
        if(remap[module] == none) {
            remap[module] = static_cast<std::uint32_t>(modules.size());
            modules.push_back(result.modules[module]);
        }
        module = remap[module];
    }
    result.modules = std::move(modules);

    auto count = result.modules.size();
    result.kinds.assign(count, ModuleKind::Program);
    result.provides.resize(count);
    result.primaries.resize(count);
    // Whether an entry rewrites the module, and whether one wraps it, as an
    // entry without a kind says.
    std::vector<bool> rewritten(count, false), wrapped(count, false);
    for(auto& declared: spec.modules) {
        // A merge or move can still take a matched module's files away.
        auto module = result.module_named(declared.name);
        if(!matched_names.contains(declared.name) || module == count) {
            continue;
        }
        // Wrapped is what an entry without a kind says; another entry of
        // the module may name its kind.
        auto& kind = result.kinds[module];
        if(declared.kind == ModuleKind::Wrapped) {
            wrapped[module] = true;
            if(kind == ModuleKind::Program) {
                kind = ModuleKind::Wrapped;
            }
        } else if(kind == ModuleKind::Program || kind == ModuleKind::Wrapped) {
            kind = declared.kind;
        } else if(kind != declared.kind) {
            return std::unexpected(
                std::format("module {} is both textual and external", declared.name));
        }
        for(auto& provided: declared.provides) {
            result.provides[module].insert(provided.getKey());
        }
        if(declared.rewrite) {
            rewritten[module] = true;
            if(!declared.primary.empty()) {
                result.primaries[module] = declared.primary;
            }
        }
    }

    // A rewritten module's primary interface defaults to the deepest
    // directory holding its headers, its files' when it has none.
    for(std::uint32_t module = 0; module < count; module += 1) {
        if(rewritten[module] && (wrapped[module] || result.kinds[module] != ModuleKind::Program)) {
            return std::unexpected(
                std::format("module {} is both rewritten and wrapped", result.modules[module]));
        }
        auto& primary = result.primaries[module];
        if(!rewritten[module] || !primary.empty()) {
            continue;
        }
        std::optional<llvm::StringRef> common;
        for(bool headers: {true, false}) {
            for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
                auto& info = facts.files[file];
                if(result.module_of[file] != module || (headers && info.source)) {
                    continue;
                }
                auto dir = path::parent(info.path, path::Style::Posix);
                if(!common) {
                    common = dir;
                }
                while(!common->empty() && !path::is_within(dir, *common)) {
                    common = path::parent(*common, path::Style::Posix);
                }
            }
            if(common) {
                break;
            }
        }
        primary = common.value_or("").str();
        path::push(primary, "module.cppm", path::Style::Posix);
    }

    // A rewritten module writes its files in place and declares its name.
    llvm::StringMap<std::uint32_t> placed;
    for(std::uint32_t module = 0; module < count; module += 1) {
        auto& primary = result.primaries[module];
        if(primary.empty()) {
            continue;
        }
        if(!is_module_name(result.modules[module])) {
            return std::unexpected(
                std::format("module {}: not a module name to rewrite", result.modules[module]));
        }
        if(auto [it, inserted] = placed.try_emplace(primary, module); !inserted) {
            auto [first, second] = std::minmax(result.modules[it->second], result.modules[module]);
            return std::unexpected(
                std::format("modules {} and {} both put their primary interface at {}",
                            first,
                            second,
                            primary));
        }
    }
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        auto module = result.module_of[file];
        if(!result.primaries[module].empty() &&
           path::is_absolute(facts.files[file].path, path::Style::Posix)) {
            return std::unexpected(std::format("module {} would rewrite {}, outside the project",
                                               result.modules[module],
                                               facts.files[file].path));
        }
    }
    return result;
}

}  // namespace clice::analysis
