#include "analysis/wrapping.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <map>
#include <utility>

#include "vfs/file_system.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Path.h"

namespace clice::analysis {

namespace {

std::string join_path(llvm::StringRef directory, llvm::StringRef name) {
    llvm::SmallString<256> path(directory);
    llvm::sys::path::append(path, llvm::sys::path::Style::posix, name);
    return path.str().str();
}

std::expected<llvm::SmallVector<std::string>, std::string> read_lines(llvm::StringRef path) {
    auto buffer = vfs::read(path);
    if(!buffer) {
        return std::unexpected(
            std::format("cannot read {}: {}", path.str(), buffer.error().message()));
    }
    llvm::SmallVector<llvm::StringRef> lines;
    (*buffer)->getBuffer().split(lines, '\n');
    llvm::SmallVector<std::string> result;
    for(auto line: lines) {
        result.push_back(line.trim().str());
    }
    return result;
}

}  // namespace

std::expected<StdModules, std::string> read_std_modules(llvm::StringRef directory) {
    StdModules result{
        .sources = {join_path(directory, "std.cppm"), join_path(directory, "std.compat.cppm")},
    };
    auto lines = read_lines(result.sources.front());
    if(!lines) {
        return std::unexpected(lines.error());
    }
    for(llvm::StringRef line: *lines) {
        if(!line.consume_front("#")) {
            continue;
        }
        line = line.ltrim();
        if(!line.consume_front("include")) {
            continue;
        }
        line = line.ltrim();
        // <__config> is libc++'s configuration, no standard header.
        if(line.consume_front("<") && !line.starts_with("__")) {
            result.headers.push_back(line.take_until([](char c) { return c == '>'; }).str());
        }
    }

    auto entries = vfs::read_dir(join_path(directory, "std.compat"));
    if(!entries) {
        return std::unexpected(std::format("cannot list {}/std.compat: {}",
                                           directory.str(),
                                           entries.error().message()));
    }
    for(auto& entry: *entries) {
        if(!llvm::StringRef(entry.path).ends_with(".inc")) {
            continue;
        }
        auto exported = read_lines(entry.path);
        if(!exported) {
            return std::unexpected(exported.error());
        }
        for(llvm::StringRef line: *exported) {
            if(line.consume_front("using ::")) {
                result.compat.insert(line.take_until([](char c) { return c == ' ' || c == ';'; }));
            }
        }
    }
    if(result.headers.empty() || result.compat.empty()) {
        return std::unexpected(
            std::format("{} holds no libc++ std and std.compat modules", directory.str()));
    }
    return result;
}

std::expected<Wrapping, std::string> wrap(const Partition& partition,
                                          llvm::ArrayRef<Interface> interfaces,
                                          const std::optional<StdModules>& libcxx,
                                          llvm::StringRef root) {
    auto absolute = [&](llvm::StringRef path) {
        return llvm::sys::path::is_absolute(path) ? path.str() : join_path(root, path);
    };
    auto operand = [&](const InterfaceHeader& header) {
        return llvm::StringRef(header.include).starts_with("<")
                   ? header.include
                   : std::format("\"{}\"", absolute(header.file));
    };

    assert(interfaces.size() == partition.modules.size());
    // The modules standing before every generated one: the standard library,
    // then the modules kept headers by name.
    std::vector<const Interface*> given;
    llvm::StringMap<const Interface*> generated;
    for(std::uint32_t module = 0; module < partition.modules.size(); module += 1) {
        auto& interface = interfaces[module];
        switch(partition.kinds[module]) {
            case ModuleKind::Program: break;
            case ModuleKind::Wrapped: generated[interface.module] = &interface; break;
            case ModuleKind::Textual: given.push_back(&interface); break;
            case ModuleKind::External:
                if(!libcxx || interface.module != "std") {
                    return std::unexpected(std::format(
                        "module {}: only std stands for an existing module, given libc++'s sources",
                        interface.module));
                }
                given.push_back(&interface);
                break;
        }
    }
    std::ranges::sort(given, [&](const Interface* lhs, const Interface* rhs) {
        auto kind = [&](const Interface* interface) {
            return partition.kinds[partition.module_named(interface->module)];
        };
        return std::pair{kind(lhs) != ModuleKind::External, lhs->module} <
               std::pair{kind(rhs) != ModuleKind::External, rhs->module};
    });

    Wrapping result;
    llvm::StringMap<std::vector<std::string>> imports;
    for(auto& [name, interface]: generated) {
        auto& list = imports[name];
        for(auto& imported: interface->imports) {
            if(generated.contains(imported)) {
                list.push_back(imported);
            } else if(partition.kinds[partition.module_named(imported)] == ModuleKind::Program) {
                result.plan.warnings.push_back(
                    std::format("{} imports the program's {}: dropped", name.str(), imported));
            }
        }
    }

    // Imported modules first; a cycle is the partition's to break.
    std::vector<std::string> order;
    llvm::StringSet<> done;
    std::vector<std::string> path;
    auto visit = [&](auto& self, llvm::StringRef name) -> std::expected<void, std::string> {
        if(done.contains(name)) {
            return {};
        }
        if(llvm::is_contained(path, name)) {
            path.push_back(name.str());
            return std::unexpected(
                std::format("modules import each other: {}", llvm::join(path, " -> ")));
        }
        path.push_back(name.str());
        for(auto& imported: imports[name]) {
            if(auto visited = self(self, imported); !visited) {
                return visited;
            }
        }
        path.pop_back();
        done.insert(name);
        order.push_back(name.str());
        return {};
    };
    std::vector<std::string> names;
    for(auto& entry: generated) {
        names.push_back(entry.first().str());
    }
    std::ranges::sort(names);
    for(auto& name: names) {
        if(auto visited = visit(visit, name); !visited) {
            return std::unexpected(visited.error());
        }
    }

    std::string base;
    for(auto* module: given) {
        for(auto& header: module->textual) {
            base += std::format("#include {}\n", operand(header));
        }
    }
    if(libcxx) {
        base += "import std.compat;\n";
    }
    for(auto* module: given) {
        base += std::format("#include \"{}.macros.h\"\n", module->module);
    }

    auto macro_header = [](const Interface& interface) {
        std::string text = "#pragma once\n";
        for(auto& macro: interface.macros) {
            text += macro.directive + "\n";
        }
        return text;
    };
    for(auto* module: given) {
        result.files.push_back({std::format("{}.macros.h", module->module), macro_header(*module)});
    }
    if(libcxx) {
        result.plan.std_sources = libcxx->sources;
        result.plan.mirrors.push_back("mirror/std");
        for(auto& header: libcxx->headers) {
            // <version> holds only macros: it stays, for the feature tests.
            if(header != "version") {
                result.files.push_back({std::format("mirror/std/{}", header), ""});
            }
        }
    }

    for(auto& name: order) {
        auto& interface = *generated[name];
        auto& module = result.plan.modules.emplace_back();
        module.name = name;
        module.source = std::format("{}.cppm", name);
        module.imports = imports[name];
        if(libcxx) {
            module.mirrors.push_back("mirror/std");
        }

        std::string unit = "module;\n\n" + base;
        for(auto& imported: module.imports) {
            unit += std::format("import {};\n", imported);
            module.mirrors.push_back(std::format("mirror/{}", imported));
        }
        for(auto& imported: module.imports) {
            unit += std::format("#include \"{}.macros.h\"\n", imported);
        }
        // Switches the program defines ahead of including the library.
        for(auto& macro: interface.reads) {
            if(partition.kinds[partition.module_named(macro.module)] == ModuleKind::Program) {
                unit += macro.directive + "\n";
            }
        }
        // What the imported modules cannot export, which their emptied
        // headers no longer bring in.
        for(auto& imported: module.imports) {
            for(auto& header: generated[imported]->textual) {
                unit += std::format("#include {}\n", operand(header));
            }
        }
        unit += "\n";
        llvm::StringSet<> roots;
        for(auto& entry: interface.entries) {
            auto file = absolute(entry.file);
            unit += std::format("#include \"{}\"\n", file);
            if(entry.name.empty()) {
                continue;
            }
            // A name relative to the includer's own directory finds the header
            // before any directory on the include path.
            llvm::SmallString<128> spelled(entry.name);
            llvm::sys::path::remove_dots(spelled, true, llvm::sys::path::Style::posix);
            if(spelled.starts_with("../") || llvm::sys::path::is_absolute(spelled)) {
                result.plan.warnings.push_back(
                    std::format("{}: {} is included as \"{}\", which no mirror can empty",
                                name,
                                entry.file,
                                entry.name));
                continue;
            }
            result.files.push_back({std::format("mirror/{}/{}", name, spelled.str()), ""});
            if(llvm::StringRef(file).ends_with(("/" + spelled).str())) {
                roots.insert(llvm::StringRef(file).drop_back(spelled.size() + 1));
            }
        }
        for(auto& entry: roots) {
            module.include_roots.push_back(entry.first().str());
        }
        std::ranges::sort(module.include_roots);

        unit += std::format("\nexport module {};\n\n", name);
        // Per enclosing namespace, its aliases then its names: no
        // using-declaration exports an alias.
        std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> scopes;
        auto scope_of = [](llvm::StringRef qualified) {
            auto separator = qualified.rfind("::");
            return separator == llvm::StringRef::npos
                       ? std::pair{llvm::StringRef(), qualified}
                       : std::pair{qualified.take_front(separator),
                                   qualified.drop_front(separator + 2)};
        };
        for(auto& alias: interface.aliases) {
            auto [scope, alias_name] = scope_of(alias.name);
            scopes[scope.str()].first.push_back(
                std::format("namespace {} = {};", alias_name.str(), alias.target));
        }
        llvm::StringSet<> exported;
        for(auto& entry: interface.exports) {
            if(exported.insert(entry.name).second) {
                scopes[scope_of(entry.name).first.str()].second.push_back(
                    std::format("using ::{};", entry.name));
            }
        }
        for(auto& [scope, lines]: scopes) {
            std::ranges::sort(lines.first);
            std::ranges::sort(lines.second);
            if(scope.empty()) {
                for(auto& line: llvm::concat<std::string>(lines.first, lines.second)) {
                    unit += std::format("export {}\n", line);
                }
                unit += "\n";
                continue;
            }
            unit += std::format("export namespace {} {{\n", scope);
            for(auto& line: llvm::concat<std::string>(lines.first, lines.second)) {
                unit += line + "\n";
            }
            unit += "}\n\n";
        }
        result.files.push_back({module.source, std::move(unit)});
        result.files.push_back({std::format("{}.macros.h", name), macro_header(interface)});
        result.plan.mirrors.push_back(std::format("mirror/{}", name));
    }

    std::string prelude = "#pragma once\n\n" + base;
    for(auto& name: order) {
        prelude += std::format("import {};\n", name);
    }
    for(auto& name: order) {
        prelude += std::format("#include \"{}.macros.h\"\n", name);
    }
    result.plan.prelude = "prelude.h";
    result.files.push_back({result.plan.prelude, std::move(prelude)});
    return result;
}

}  // namespace clice::analysis
