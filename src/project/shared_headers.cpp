module;

#include "support/logging.macros.h"

module clice;

import :command.argument;
import :compile.compilation;
import :project.graph.pch;
import :project.graph.precompiled;
import :project.project;
import :project.shared_headers;
import :support.logging;
import :syntax.lexical_scan;
import :syntax.preamble;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// How many units of a pass, compiled under one command, must load a
/// shared header for it to pay: its build parses all it holds, and so
/// does its own index.
constexpr std::size_t least_units = 8;

/// What share of a group's units a shared header's every file reaches, from
/// the headers all of them include down to those half of them do: a unit
/// loads the deepest whose files it includes all, so that no header gives
/// it a file it does not include.
constexpr double levels[] = {1, 0.95, 0.9, 0.8, 0.7, 0.6, 0.5};

/// Whether `file` is guarded, `#pragma once` or an include guard: a unit
/// including it skips it once the PCH holds it.
bool guarded(llvm::StringRef file, const clang::LangOptions& dialect) {
    auto content = vfs::read_text(file);
    if(!content) {
        return false;
    }
    llvm::StringRef text = *content;
    return has_pragma_once(text, static_cast<std::uint32_t>(text.size())) ||
           lexical_scan(text, dialect).include_guard.has_value();
}

/// The files of `unit`'s closure in the order its compile first enters them,
/// as the scan reads its includes, conditional or not.
llvm::DenseMap<Fid, std::uint32_t> entry_order(const DependencyGraph& graph, Fid unit) {
    llvm::DenseMap<Fid, std::uint32_t> order;
    auto visit = [&](auto& self, Fid file) -> void {
        if(!order.try_emplace(file, static_cast<std::uint32_t>(order.size())).second) {
            return;
        }
        for(auto& context: graph.contexts(file)) {
            for(auto edge: graph.get_includes(file, context.group)) {
                self(self, edge.fid);
            }
        }
    };
    visit(visit, unit);
    return order;
}

}  // namespace

void Project::plan_shared_headers(llvm::ArrayRef<Fid> units) {
    release_shared_headers();
    if(!settings.project.shared_pch || !store) {
        return;
    }

    struct Group {
        CommandRef command;

        std::vector<Fid> units;
    };

    llvm::MapVector<std::pair<std::uint64_t, const char*>, Group> groups;
    for(auto unit: units) {
        if(!is_unit(unit) || !dep_graph.module_of(unit).empty()) {
            continue;
        }
        auto selection = select_command(unit);
        auto& ref = selection.ref;
        auto language = llvm::StringRef(ref.language.value);
        if(ref.source != CommandSource::Database || (language != "c++" && language != "c") ||
           commands.pch_output(ref.command) || pch_writer(unit, selection)) {
            continue;
        }
        auto& group = groups[{commands.command_hash(ref.command), ref.language.value}];
        if(group.units.empty()) {
            group.command = ref;
        }
        group.units.push_back(unit);
    }

    auto directory = path::join(store->base_dir(), "shared");
    for(auto& [key, group]: groups) {
        if(group.units.size() < least_units) {
            continue;
        }
        ranges::sort(group.units, {}, [&](Fid unit) { return file_table.resolve(unit); });
        group.units.erase(ranges::unique(group.units).begin(), group.units.end());
        std::vector<llvm::DenseSet<Fid>> closures;
        llvm::DenseMap<Fid, std::uint32_t> reached;
        for(auto unit: group.units) {
            auto& closure = closures.emplace_back(dep_graph.include_closure(unit));
            for(auto file: closure) {
                reached[file] += 1;
            }
        }
        // A unit's own text and what its command forces in hold nothing to
        // share.
        llvm::DenseSet<Fid> excluded(group.units.begin(), group.units.end());
        for(auto unit: group.units) {
            auto forced = dep_graph.get_forced_includes(unit);
            excluded.insert(forced.begin(), forced.end());
        }

        // The distinct levels, each the files enough of the units reach.
        std::vector<std::vector<Fid>> held;
        for(auto level: levels) {
            auto least = static_cast<std::uint32_t>(std::ceil(level * group.units.size()));
            std::vector<Fid> files;
            for(auto [file, count]: reached) {
                if(count >= least && !excluded.contains(file)) {
                    files.push_back(file);
                }
            }
            ranges::sort(files);
            if(!files.empty() && (held.empty() || held.back() != files)) {
                held.push_back(std::move(files));
            }
        }
        std::vector<std::vector<Fid>> assigned(held.size());
        for(auto [unit, closure]: views::zip(group.units, closures)) {
            for(auto level = held.size(); level-- > 0;) {
                if(ranges::all_of(held[level], [&](Fid file) { return closure.contains(file); })) {
                    assigned[level].push_back(unit);
                    break;
                }
            }
        }

        auto& dialect = this->dialect(group.command.file, nullptr);
        for(auto level: views::iota(std::size_t(0), held.size())) {
            auto& members = assigned[level];
            if(members.size() < least_units) {
                continue;
            }
            // The headers no other of them includes, guarded, in the order
            // a member enters them.
            llvm::DenseMap<Fid, llvm::DenseSet<Fid>> reaches;
            for(auto file: held[level]) {
                reaches[file] = dep_graph.include_closure(file);
            }
            auto candidates = held[level];
            ranges::stable_sort(candidates, std::greater{}, [&](Fid file) {
                return reaches[file].size();
            });
            llvm::DenseSet<Fid> covered;
            std::vector<Fid> roots;
            for(auto file: candidates) {
                if(covered.contains(file) || !guarded(file_table.resolve(file), dialect)) {
                    continue;
                }
                roots.push_back(file);
                covered.insert(reaches[file].begin(), reaches[file].end());
            }
            if(roots.empty()) {
                continue;
            }
            auto order = entry_order(dep_graph, members.front());
            ranges::sort(roots, {}, [&](Fid file) { return order.lookup(file); });

            std::string text = "#pragma once\n";
            for(auto root: roots) {
                text += std::format("#include \"{}\"\n",
                                    llvm::sys::path::convert_to_slash(file_table.resolve(root)));
            }
            auto name = std::format("{:016x}.{}",
                                    llvm::xxh3_64bits(std::format("{}:{}:{}",
                                                                  key.first,
                                                                  key.second,
                                                                  level)),
                                    llvm::StringRef(key.second) == "c" ? "h" : "hxx");
            auto file = path::join(directory, name);
            if(auto made = fs::create_directories(directory); !made) {
                LOG_WARN("Cannot create {} for the shared headers: {}",
                         directory,
                         made.error().message());
                return;
            }
            if(auto error = fs::write_file_atomic(file, text)) {
                LOG_WARN("Cannot write the shared header {}: {}", file, error.message());
                continue;
            }

            auto rendered = toolchain.render(group.command, file);
            std::vector<std::string> arguments(rendered.argv.begin(), rendered.argv.end() - 1);
            for(auto it = arguments.begin(); it != arguments.end();) {
                it = *it == "-x" && it + 1 != arguments.end() ? arguments.erase(it, it + 2) : it + 1;
            }
            arguments.push_back("-x");
            arguments.push_back(std::format("{}-header", key.second));
            arguments.push_back(file);
            PCHRequest request{
                .params = {.file = file,
                           .directory = commands.get(group.command.command).directory,
                           .arguments = std::move(arguments),
                           .hidden = std::move(rendered.hidden),
                           .whole_file = true,
                           .shared = true},
            };
            auto& params = request.params;
            request.pch_key = cache_key({clang::getClangFullVersion(),
                                         pch_build_rule(false),
                                         database_key.str(),
                                         params.directory,
                                         params.file,
                                         text,
                                         canonicalize(params.arguments)});
            auto header = file_table.intern(Spelling::absolute(file));
            request.writer = PCHRequest::Writer{header, llvm::xxh3_64bits(request.pch_key)};
            auto index = static_cast<std::uint32_t>(shared_headers.headers.size());
            shared_headers.headers.push_back({.file = header, .request = std::move(request)});
            shared_headers.files[header] = index;
            for(auto unit: members) {
                shared_headers.units[unit] = index;
            }
            LOG_INFO("Shared header {}: {} units load {} headers through {} includes",
                     file,
                     members.size(),
                     covered.size(),
                     roots.size());
        }
    }
}

std::optional<PCHRequest> Project::shared_header_request(Fid unit) const {
    auto it = shared_headers.units.find(unit);
    if(it == shared_headers.units.end()) {
        return std::nullopt;
    }
    return shared_headers.headers[it->second].request;
}

void Project::release_shared_headers() {
    index::IndexReport report;
    for(auto& header: shared_headers.headers) {
        if(index.indexed(header.file)) {
            index.drop(header.file, report);
        }
        discard_pch(header.request.pch_key);
        fs::remove(header.request.params.file);
    }
    take_report(report);
    shared_headers = SharedHeaders();
}

}  // namespace clice
