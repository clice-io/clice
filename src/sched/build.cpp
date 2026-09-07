#include "sched/build.h"

#include <cassert>
#include <format>

#include "sched/configuration.h"
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

void Build::reset_active(llvm::StringRef configuration) {
    assert(configuration.empty() ? config.configurations().empty()
                                 : declares_configuration(config, configuration));
    claimed_sources.reset();
    active = configuration.str();
}

llvm::SmallVector<const CompiledRule*> Build::matching(llvm::StringRef path) const {
    return config.matching_rules(path, active);
}

static bool rule_active(const CompiledRule& rule, llvm::StringRef active) {
    return rule.configuration.empty() || rule.configuration == active;
}

bool Build::declares_sources() const {
    return llvm::any_of(config.compiled_rules, [&](const CompiledRule& rule) {
        return rule_active(rule, active) && rule.declares_sources();
    });
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

llvm::SmallVector<SourceID, 4> Build::declared_ids() const {
    llvm::SmallVector<SourceID, 4> declared;
    bool declares = declares_sources();
    for(auto& rule: config.compiled_rules) {
        if(rule_active(rule, active) || declares) {
            for(auto& database: rule.compile_commands) {
                if(auto id = cdb.find_source(database)) {
                    declared.push_back(*id);
                }
            }
        }
    }
    return declared;
}

bool Build::discovered(SourceID id) const {
    return !llvm::is_contained(declared_ids(), id);
}

llvm::SmallVector<SourceID, 4> Build::source_order(llvm::StringRef path) const {
    auto matched = matching(path);
    llvm::SmallVector<SourceID, 4> order;
    auto declared = declared_ids();
    auto add_sources = [&](const CompiledRule& rule) {
        for(auto& database: rule.compile_commands) {
            if(auto id = cdb.find_source(database); id && !llvm::is_contained(order, *id)) {
                order.push_back(*id);
            }
        }
    };
    for(auto& rule: config.compiled_rules) {
        if(rule_active(rule, active) && llvm::is_contained(matched, &rule)) {
            add_sources(rule);
        }
    }
    for(auto& rule: config.compiled_rules) {
        if(rule_active(rule, active) && !llvm::is_contained(matched, &rule)) {
            add_sources(rule);
        }
    }
    llvm::SmallVector<SourceID, 4> discovered;
    for(std::size_t i = 0; i < cdb.source_count(); i += 1) {
        auto id = SourceID(i);
        if(!llvm::is_contained(order, id) && !llvm::is_contained(declared, id)) {
            discovered.push_back(id);
        }
    }
    // A vanished database keeps serving its entries, but one regenerated
    // elsewhere takes over the files both list.
    auto rank = [&](SourceID id) {
        auto source = cdb.source_path(id);
        auto depth = llvm::count_if(source, [](char c) { return path::is_separator(c); });
        return std::tuple(!cdb.present(id), depth, source);
    };
    std::ranges::sort(discovered, {}, rank);
    order.append(discovered);
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

const CompiledRule* Build::default_rule(llvm::StringRef path) const {
    for(auto* rule: matching(path)) {
        if(rule->has_default_command()) {
            return rule;
        }
    }
    return nullptr;
}

std::optional<ConfigID> Build::command_of(const CompiledRule& rule) {
    llvm::SmallVector<const char*, 16> argv;
    for(auto& arg: rule.default_command) {
        argv.push_back(arg.c_str());
    }
    return cdb.intern_command(rule.directory, argv);
}

std::optional<ConfigID> Build::default_command(llvm::StringRef path) {
    auto* rule = default_rule(path);
    return rule ? command_of(*rule) : std::nullopt;
}

ConfigID Build::builtin(llvm::StringRef path) {
    // Every C++ spelling (.cc, .cxx, .C, .hh) gets clang++, and so does the
    // ambiguous .h; C, Objective-C and unknown extensions get clang.
    namespace types = clang::driver::types;
    auto type = suffix_type(path);
    llvm::SmallVector<const char*, 8> arguments;
    if(path::extension(path) == ".cuh" || (type != types::TY_INVALID && types::isCuda(type))) {
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
    if(!claimed_sources) {
        claimed_sources.emplace();
        enumerate_default_sources(*claimed_sources);
    }
    for(auto file: *claimed_sources) {
        if(seen.insert(file).second) {
            result.push_back(file);
        }
    }
    return result;
}

Build::DefaultSourcesRefresh Build::refresh_default_sources() {
    std::vector<Fid> current;
    enumerate_default_sources(current);
    DefaultSourcesRefresh refresh;
    if(claimed_sources) {
        llvm::DenseSet<Fid> known(claimed_sources->begin(), claimed_sources->end());
        llvm::DenseSet<Fid> now(current.begin(), current.end());
        for(auto file: current) {
            if(!known.contains(file)) {
                refresh.appeared.push_back(file);
            }
        }
        for(auto file: *claimed_sources) {
            if(!now.contains(file)) {
                refresh.vanished.push_back(file);
            }
        }
    }
    claimed_sources = std::move(current);
    return refresh;
}

bool Build::default_source(llvm::StringRef path) {
    namespace types = clang::driver::types;
    // Every C-family input clang compiles as a unit, preprocessed and module
    // interface files included; a header claims no translation unit of its
    // own.
    auto type = suffix_type(path);
    if(type != types::TY_INVALID) {
        return types::isDerivedFromC(type) && !types::onlyPrecompileType(type);
    }
    if(path::extension(path) == ".cuh") {
        return false;
    }
    auto* rule = default_rule(path);
    if(!rule || rule->patterns.empty()) {
        return false;
    }
    auto command = command_of(*rule);
    if(!command) {
        return false;
    }
    auto forced = cdb.forced_language(*command);
    return !forced.empty() && !forced.ends_with("-header");
}

bool Build::unit(Fid file) {
    if(!entries(file).empty()) {
        return true;
    }
    auto path = files.resolve(file);
    return default_command(path).has_value() && default_source(path);
}

void Build::enumerate_default_sources(std::vector<Fid>& out) {
    llvm::SmallVector<const CompiledRule*> claimants;
    for(auto& rule: config.compiled_rules) {
        if(rule_active(rule, active) && rule.has_default_command() && !rule.unmatchable) {
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
            return other != root && path::under(root, other);
        });
    });

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
            if(!default_source(entry_path)) {
                continue;
            }
            auto file = files.intern(entry_path);
            if(seen.insert(file).second) {
                out.push_back(file);
            }
        }
    }
}

}  // namespace clice
