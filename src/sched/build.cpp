#include "sched/build.h"

#include <format>

#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"
#include "clang/Driver/Types.h"

namespace clice {

void Build::reset_active() {
    auto tags = config.configurations();
    llvm::StringRef preferred = config.default_configuration;
    if(!preferred.empty() && llvm::is_contained(tags, preferred)) {
        active = preferred.str();
    } else {
        active = tags.empty() ? std::string() : tags.front().str();
    }
}

llvm::SmallVector<const CompiledRule*> Build::matching(llvm::StringRef path) const {
    return config.matching_rules(path, active);
}

static bool rule_active(const CompiledRule& rule, llvm::StringRef active) {
    return rule.configuration.empty() || rule.configuration == active;
}

llvm::SmallVector<llvm::StringRef> Build::declared_sources() const {
    llvm::SmallVector<llvm::StringRef> result;
    for(auto& rule: config.compiled_rules) {
        if(!rule_active(rule, active)) {
            continue;
        }
        for(auto& database: rule.compile_commands) {
            if(!llvm::is_contained(result, llvm::StringRef(database))) {
                result.push_back(database);
            }
        }
    }
    return result;
}

llvm::SmallVector<SourceID, 4> Build::source_order(llvm::StringRef path) const {
    auto matched = matching(path);
    llvm::SmallVector<SourceID, 4> order;
    llvm::SmallVector<SourceID, 4> declared;
    auto add_sources = [&](const CompiledRule& rule) {
        for(auto& database: rule.compile_commands) {
            if(auto id = cdb.find_source(database); id && !llvm::is_contained(order, *id)) {
                order.push_back(*id);
            }
        }
    };
    for(auto& rule: config.compiled_rules) {
        for(auto& database: rule.compile_commands) {
            if(auto id = cdb.find_source(database)) {
                declared.push_back(*id);
            }
        }
        if(rule_active(rule, active) && llvm::is_contained(matched, &rule)) {
            add_sources(rule);
        }
    }
    for(auto& rule: config.compiled_rules) {
        if(rule_active(rule, active) && !llvm::is_contained(matched, &rule)) {
            add_sources(rule);
        }
    }
    for(std::size_t i = 0; i < cdb.source_count(); i += 1) {
        auto id = SourceID(i);
        if(!llvm::is_contained(order, id) && !llvm::is_contained(declared, id)) {
            order.push_back(id);
        }
    }
    return order;
}

llvm::SmallVector<CompilationEntry, 2> Build::entries(Fid file) const {
    auto all = cdb.candidate_entries(file);
    if(all.empty()) {
        return {};
    }
    llvm::SmallVector<CompilationEntry, 2> result;
    for(auto id: source_order(files.resolve(file))) {
        for(auto& entry: all) {
            if(entry.source == id) {
                result.push_back(entry);
            }
        }
    }
    return result;
}

llvm::SmallVector<Candidate, 2> Build::commands(Fid file) {
    llvm::SmallVector<Candidate, 2> result;
    for(auto& entry: entries(file)) {
        result.push_back({.config = entry.config, .source = CommandSource::CDBExact});
    }
    if(result.empty()) {
        if(auto id = default_command(files.resolve(file))) {
            result.push_back({.config = *id, .source = CommandSource::Default});
        }
    }
    return result;
}

Edits Build::edits(llvm::ArrayRef<llvm::StringRef> paths) const {
    llvm::SmallVector<const CompiledRule*> matched;
    for(auto path: paths) {
        for(auto* rule: matching(path)) {
            if(!llvm::is_contained(matched, rule)) {
                matched.push_back(rule);
            }
        }
    }

    Edits result;
    for(auto& rule: config.compiled_rules) {
        if(!llvm::is_contained(matched, &rule)) {
            continue;
        }
        if(!rule.remove.empty()) {
            result.edits.push_back({.kind = CommandEdit::Kind::Remove, .flags = rule.remove});
        }
        if(!rule.append.empty()) {
            result.edits.push_back({.kind = CommandEdit::Kind::Append, .flags = rule.append});
        }
    }
    return result;
}

std::optional<ConfigID> Build::default_command(llvm::StringRef path) {
    for(auto* rule: matching(path)) {
        if(!rule->has_default_command()) {
            continue;
        }
        llvm::SmallVector<const char*, 16> argv;
        for(auto& arg: rule->default_command) {
            argv.push_back(arg.c_str());
        }
        return cdb.intern_command(rule->directory, argv);
    }
    return std::nullopt;
}

ConfigID Build::builtin(llvm::StringRef path) {
    // Every C++ spelling (.cc, .cxx, .C, .hh) gets clang++, and so does the
    // ambiguous .h; C, Objective-C and unknown extensions get clang.
    namespace types = clang::driver::types;
    auto ext = path::extension(path);
    ext.consume_front(".");
    auto type = ext.empty() ? types::TY_INVALID : types::lookupTypeForExtension(ext);
    llvm::SmallVector<const char*, 8> arguments;
    if(ext == "cu" || ext == "cuh" || (type != types::TY_INVALID && types::isCuda(type))) {
        // Device-only pins the same device-side view NVCC-backed commands
        // default to, instead of whichever job the toolchain query happens
        // to pick from a two-sided compilation; a rule appending
        // --cuda-host-only still wins as the later flag.
        arguments = {"clang++", "-std=c++20", "-x", "cuda", "--cuda-device-only"};
    } else if(type != types::TY_INVALID && types::isCXX(type)) {
        arguments = {"clang++", "-std=c++20"};
    } else if(type == types::TY_CHeader) {
        // C++ by default, like clangd; -x forces TU semantics instead of a
        // precompiled-header job.
        arguments = {"clang++", "-std=c++20", "-x", "c++"};
    } else {
        arguments = {"clang"};
    }
    return *cdb.intern_command("", arguments);
}

CommandRef Build::resolve(Fid file,
                          ConfigID base,
                          CommandSource source,
                          llvm::ArrayRef<llvm::StringRef> paths,
                          llvm::StringRef language_path,
                          llvm::ArrayRef<std::string> extra_prepend,
                          llvm::ArrayRef<std::string> extra_append) {
    auto edit = edits(paths);
    auto applied = cdb.apply_rules(base, edit.options(extra_prepend, extra_append));
    return {file, applied, cdb.input_kind(applied, language_path), source};
}

std::string Build::edit_hash(llvm::ArrayRef<llvm::StringRef> paths) const {
    auto edit = edits(paths);
    if(edit.empty()) {
        return {};
    }
    std::string joined;
    for(auto& item: edit.edits) {
        joined += item.kind == CommandEdit::Kind::Remove ? 'r' : 'a';
        for(auto& flag: item.flags) {
            joined += flag;
            joined += '\0';
        }
        joined += '\1';
    }
    return std::format("{:016x}", llvm::xxh3_64bits(joined));
}

bool Build::indexed(llvm::StringRef path) const {
    return llvm::all_of(matching(path), [](const CompiledRule* rule) { return rule->index; });
}

llvm::SmallVector<CommandRef> Build::units(llvm::ArrayRef<Fid> members) {
    llvm::SmallVector<CommandRef> result;
    for(auto member: members) {
        auto path = files.resolve(member);
        // Two databases listing the file with the same command make one
        // unit: the scan would only read it twice.
        auto first = result.size();
        for(auto& command: commands(member)) {
            auto unit = resolve(member, command.config, command.source, path, path);
            bool seen = llvm::any_of(llvm::ArrayRef(result).drop_front(first),
                                     [&](const CommandRef& other) {
                                         return other.config == unit.config &&
                                                other.input.value == unit.input.value;
                                     });
            if(!seen) {
                result.push_back(unit);
            }
        }
    }
    return result;
}

std::vector<Fid> Build::members() {
    std::vector<Fid> result;
    llvm::DenseSet<Fid> seen;
    for(auto& entry: cdb.entries()) {
        if(seen.insert(entry.file).second && !entries(entry.file).empty()) {
            result.push_back(entry.file);
        }
    }
    enumerate_default_sources(result);
    return result;
}

/// Whether `path` is `root` or lies under it.
static bool under(llvm::StringRef path, llvm::StringRef root) {
    return path == root || (path.starts_with(root) &&
                            (root.ends_with("/") || path::is_separator(path[root.size()])));
}

void Build::enumerate_default_sources(std::vector<Fid>& out) {
    llvm::SmallVector<const CompiledRule*> claimants;
    for(auto& rule: config.compiled_rules) {
        if(rule_active(rule, active) && rule.has_default_command()) {
            claimants.push_back(&rule);
        }
    }
    if(claimants.empty()) {
        return;
    }

    // Where the claimed files can be: each pattern's literal directory, the
    // whole workspace for a rule without patterns. A root inside another
    // is walked as part of it.
    llvm::SmallVector<llvm::StringRef> roots;
    auto add_root = [&](llvm::StringRef root) {
        if(!root.empty() && !llvm::is_contained(roots, root)) {
            roots.push_back(root);
        }
    };
    for(auto* rule: claimants) {
        if(rule->patterns.empty()) {
            add_root(config.workspace_root);
        }
        for(auto& pattern: rule->patterns) {
            add_root(pattern.root);
        }
    }
    llvm::erase_if(roots, [&](llvm::StringRef root) {
        return llvm::any_of(roots, [&](llvm::StringRef other) {
            return other != root && under(root, other);
        });
    });

    namespace types = clang::driver::types;
    llvm::DenseSet<Fid> seen(out.begin(), out.end());
    llvm::StringRef cache_dir = config.project.cache_dir;
    for(auto root: roots) {
        std::error_code ec;
        for(llvm::sys::fs::recursive_directory_iterator it(root, ec, /*follow_symlinks=*/false),
            end;
            it != end;
            it.increment(ec)) {
            // An unreadable directory is skipped, not the rest of the walk.
            if(ec) {
                LOG_WARN("Cannot read a directory under {}: {}", root, ec.message());
                ec.clear();
                continue;
            }
            // The iterator spells paths natively; the cache directory and
            // the patterns are canonical.
            llvm::SmallString<256> storage;
            auto entry_path = path::canonical(it->path(), storage);
            if(it->type() == llvm::sys::fs::file_type::directory_file) {
                if(path::filename(entry_path) == ".git" || entry_path == cache_dir) {
                    it.no_push();
                }
                continue;
            }
            auto matched = matching(entry_path);
            if(!llvm::any_of(claimants, [&](const CompiledRule* rule) {
                   return llvm::is_contained(matched, rule);
               })) {
                continue;
            }
            // Sources only — every C-family input clang compiles as a unit,
            // preprocessed and module interface files included; a header
            // claims no translation unit of its own. A suffix clang does not
            // know is a source when the default command forces its language
            // (`-x c++` for an extensionless tool).
            auto ext = path::extension(entry_path);
            ext.consume_front(".");
            auto type = ext.empty() ? types::TY_INVALID : types::lookupTypeForExtension(ext);
            bool source = type != types::TY_INVALID && types::isDerivedFromC(type) &&
                          !types::onlyPrecompileType(type);
            if(!source) {
                if(type != types::TY_INVALID) {
                    continue;
                }
                auto command = default_command(entry_path);
                if(!command) {
                    continue;
                }
                auto forced = cdb.forced_language(*command);
                if(forced.empty() || forced.ends_with("-header")) {
                    continue;
                }
            }
            auto file = files.intern(entry_path);
            if(seen.insert(file).second) {
                out.push_back(file);
            }
        }
    }
}

}  // namespace clice
