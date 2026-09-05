#include "sched/build_view.h"

#include <format>
#include <variant>

#include "support/filesystem.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"
#include "clang/Driver/Types.h"

namespace clice {

void BuildView::reset_active() {
    auto tags = config.configurations();
    llvm::StringRef preferred = config.default_configuration;
    if(!preferred.empty() && llvm::is_contained(tags, preferred)) {
        active = preferred.str();
    } else {
        active = tags.empty() ? std::string() : tags.front().str();
    }
}

llvm::SmallVector<const CompiledRule*> BuildView::matching(llvm::StringRef path) const {
    return config.matching_rules(path, active);
}

static bool rule_active(const CompiledRule& rule, llvm::StringRef active) {
    return rule.configuration.empty() || rule.configuration == active;
}

llvm::SmallVector<llvm::StringRef> BuildView::declared_sources() const {
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

llvm::SmallVector<CompilationEntry, 2> BuildView::candidates(Fid file) const {
    auto all = cdb.candidate_entries(file);
    if(all.empty()) {
        return {};
    }

    // Source priority: the sources of rules matching the file, then those
    // of the other active rules, both in declaration order; sources no rule
    // declares (discovered ones, the test source) last. A source only
    // inactive rules declare stays out.
    auto path = files.resolve(file);
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
    for(auto& entry: all) {
        if(!llvm::is_contained(order, entry.source) &&
           !llvm::is_contained(declared, entry.source)) {
            order.push_back(entry.source);
        }
    }

    llvm::SmallVector<CompilationEntry, 2> result;
    for(auto id: order) {
        for(auto& entry: all) {
            if(entry.source == id) {
                result.push_back(entry);
            }
        }
    }
    return result;
}

llvm::SmallVector<Candidate, 2> BuildView::commands(Fid file) {
    llvm::SmallVector<Candidate, 2> result;
    for(auto& entry: candidates(file)) {
        result.push_back({.config = entry.config, .source = CommandSource::CDBExact});
    }
    if(result.empty()) {
        if(auto id = default_command(files.resolve(file))) {
            result.push_back({.config = *id, .source = CommandSource::Default});
        }
    }
    return result;
}

Edits BuildView::edits(llvm::ArrayRef<llvm::StringRef> paths) const {
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
        // A later rule's remove also cancels what an earlier rule appended:
        // apply_rules removes from the base command only.
        for(auto& flag: rule.remove) {
            std::erase(result.append, flag);
            result.remove.push_back(flag);
        }
        result.append.insert(result.append.end(), rule.append.begin(), rule.append.end());
    }
    return result;
}

std::optional<ConfigID> BuildView::default_command(llvm::StringRef path) {
    for(auto* rule: matching(path)) {
        if(!rule->has_default_command()) {
            continue;
        }
        return std::visit(
            [&](const auto& spelling) {
                if constexpr(std::is_same_v<std::decay_t<decltype(spelling)>, std::string>) {
                    return cdb.intern_command_line(rule->directory, spelling);
                } else {
                    llvm::SmallVector<const char*, 16> argv;
                    for(auto& arg: spelling) {
                        argv.push_back(arg.c_str());
                    }
                    return cdb.intern_command(rule->directory, argv);
                }
            },
            rule->default_command);
    }
    return std::nullopt;
}

ConfigID BuildView::builtin(llvm::StringRef path) {
    // Every C++ spelling (.cc, .cxx, .C, .hh) gets clang++; C, Objective-C
    // and unknown extensions get clang.
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
    } else {
        arguments = {"clang"};
    }
    return *cdb.intern_command("", arguments);
}

CommandRef BuildView::resolve(Fid file,
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

std::string BuildView::edit_hash(llvm::ArrayRef<llvm::StringRef> paths) const {
    auto edit = edits(paths);
    if(edit.append.empty() && edit.remove.empty()) {
        return {};
    }
    std::string joined;
    for(auto& arg: edit.append) {
        joined += 'a';
        joined += arg;
        joined += '\0';
    }
    for(auto& arg: edit.remove) {
        joined += 'r';
        joined += arg;
        joined += '\0';
    }
    return std::format("{:016x}", llvm::xxh3_64bits(joined));
}

bool BuildView::indexed(llvm::StringRef path) const {
    return llvm::all_of(matching(path), [](const CompiledRule* rule) { return rule->index; });
}

std::vector<Fid> BuildView::members() const {
    std::vector<Fid> result;
    llvm::DenseSet<Fid> seen;
    for(auto& entry: cdb.entries()) {
        if(seen.insert(entry.file).second && !candidates(entry.file).empty()) {
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

void BuildView::enumerate_default_sources(std::vector<Fid>& out) const {
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
        for(llvm::sys::fs::recursive_directory_iterator it(root, ec), end; it != end && !ec;
            it.increment(ec)) {
            llvm::StringRef entry_path = it->path();
            if(it->type() == llvm::sys::fs::file_type::directory_file) {
                if(path::filename(entry_path) == ".git" ||
                   (!cache_dir.empty() && entry_path == cache_dir)) {
                    it.no_push();
                }
                continue;
            }
            // Sources only: a header claims no translation unit of its own.
            auto ext = path::extension(entry_path);
            ext.consume_front(".");
            auto type = ext.empty() ? types::TY_INVALID : types::lookupTypeForExtension(ext);
            if(type == types::TY_INVALID || types::onlyPrecompileType(type) ||
               !(types::isCXX(type) || type == types::TY_C || types::isObjC(type) ||
                 types::isCuda(type))) {
                continue;
            }
            std::string canonical = entry_path.str();
            path::canonicalize(canonical);
            auto matched = matching(canonical);
            if(!llvm::any_of(claimants, [&](const CompiledRule* rule) {
                   return llvm::is_contained(matched, rule);
               })) {
                continue;
            }
            auto file = files.intern(canonical);
            if(seen.insert(file).second) {
                out.push_back(file);
            }
        }
    }
}

}  // namespace clice
