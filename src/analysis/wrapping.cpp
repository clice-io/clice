module;

#include "modules/prelude.h"

module clice;

import :analysis.wrapping;
import :vfs.file_system;

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

/// The modules, imported ones first and by name otherwise; a cycle is the
/// partition's to break.
std::expected<std::vector<std::string>, std::string>
    import_order(const llvm::StringMap<std::vector<std::string>>& imports) {
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
        for(auto& imported: imports.at(name)) {
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
    for(auto& entry: imports) {
        names.push_back(entry.first().str());
    }
    std::ranges::sort(names);
    for(auto& name: names) {
        if(auto visited = visit(visit, name); !visited) {
            return std::unexpected(visited.error());
        }
    }
    return order;
}

/// A module's exports by enclosing namespace: its aliases, then its names,
/// as no using-declaration exports an alias.
using Exports = std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>>;

std::string export_text(const Exports& exports) {
    std::string text;
    for(auto& [scope, lines]: exports) {
        if(!text.empty()) {
            text += "\n";
        }
        auto all = llvm::concat<const std::string>(lines.first, lines.second);
        if(scope.empty()) {
            for(auto& line: all) {
                text += std::format("export {}\n", line);
            }
            continue;
        }
        text += std::format("export namespace {} {{\n", scope);
        for(auto& line: all) {
            text += line + "\n";
        }
        text += "}\n";
    }
    return text;
}

/// A module unit `wrap` wrote: its global module fragment and its exports.
struct Wrapper {
    std::string fragment;
    Exports exports;
};

std::expected<Wrapper, std::string> read_wrapper(llvm::StringRef module, llvm::StringRef text) {
    auto declaration = std::format("\nexport module {};\n", module.str());
    auto split = text.find(declaration);
    if(!text.starts_with("module;\n") || split == llvm::StringRef::npos) {
        return std::unexpected("no module unit modularize writes");
    }
    Wrapper unit{.fragment = text.slice(8, split).trim().str() + "\n"};
    llvm::SmallVector<llvm::StringRef> lines;
    text.drop_front(split + declaration.size()).split(lines, '\n', -1, false);
    std::optional<std::string> scope;
    for(auto line: lines) {
        if(scope && line == "}") {
            scope.reset();
            continue;
        }
        if(!scope) {
            if(!line.consume_front("export ")) {
                return std::unexpected(std::format("unexpected line: {}", line.str()));
            }
            if(line.starts_with("namespace ") && line.ends_with(" {")) {
                scope = line.drop_front(10).drop_back(2).str();
                continue;
            }
        }
        auto& [aliases, names] = unit.exports[scope.value_or("")];
        if(line.starts_with("namespace ")) {
            aliases.insert(line.str());
        } else if(line.starts_with("using ")) {
            names.insert(line.str());
        } else {
            return std::unexpected(std::format("unexpected line: {}", line.str()));
        }
    }
    if(scope) {
        return std::unexpected(std::format("namespace {} is not closed", *scope));
    }
    return unit;
}

void drop_mirrors(Wrapping& wrapping) {
    std::erase_if(wrapping.files,
                  [](auto& file) { return llvm::StringRef(file.path).starts_with("mirror/"); });
    wrapping.plan.mirrors.clear();
    for(auto& module: wrapping.plan.modules) {
        module.mirrors.clear();
    }
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
    if(result.headers.empty() || result.compat.empty() || !vfs::is_file(result.sources.back())) {
        return std::unexpected(
            std::format("{} holds no libc++ std and std.compat modules", directory.str()));
    }
    return result;
}

std::expected<Wrapping, std::string> wrap(const Partition& partition,
                                          llvm::ArrayRef<Interface> interfaces,
                                          const std::optional<StdModules>& libcxx,
                                          llvm::StringRef root,
                                          bool mirrors) {
    auto absolute = [&](llvm::StringRef path) {
        return llvm::sys::path::is_absolute(path) ? path.str() : join_path(root, path);
    };
    // By its name where programs include it by one, else by its path.
    auto operand = [&](const InterfaceHeader& header) {
        return header.include == std::format("\"{}\"", header.file)
                   ? std::format("\"{}\"", absolute(header.file))
                   : header.include;
    };

    assert(interfaces.size() == partition.modules.size());
    // The modules standing before every generated one: the standard library,
    // then the modules kept headers by name.
    std::vector<const Interface*> given;
    llvm::StringMap<const Interface*> generated;
    for(std::uint32_t module = 0; module < partition.modules.size(); module += 1) {
        auto& interface = interfaces[module];
        auto kind = partition.kinds[module];
        if((kind == ModuleKind::Wrapped || kind == ModuleKind::Textual) &&
           (!is_module_name(interface.module) || interface.module == "std" ||
            interface.module == "std.compat")) {
            return std::unexpected(
                std::format("module {}: not a module name to generate", interface.module));
        }
        switch(kind) {
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

    auto order = import_order(imports);
    if(!order) {
        return std::unexpected(order.error());
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

    for(auto& name: *order) {
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
        llvm::StringSet<> roots;
        // The names a mirror can shadow: inside it, as no `..` or absolute
        // path is; each also tells the root the header is found under.
        auto mirrorable = [&](const InterfaceHeader& header) {
            llvm::SmallVector<std::string> names;
            for(auto& name: header.names) {
                llvm::SmallString<128> spelled(name);
                llvm::sys::path::remove_dots(spelled, true, llvm::sys::path::Style::posix);
                if(spelled.empty() || spelled.starts_with("../") || spelled.contains('\\') ||
                   llvm::sys::path::is_absolute(spelled)) {
                    continue;
                }
                auto file = absolute(header.file);
                if(llvm::StringRef(file).ends_with(("/" + spelled).str())) {
                    roots.insert(llvm::StringRef(file).drop_back(spelled.size() + 1));
                }
                names.push_back(spelled.str().str());
            }
            return names;
        };
        // What the imported modules cannot export that its headers name,
        // which the emptied headers no longer bring in.
        for(auto& header: interface.textual_uses) {
            unit += std::format("#include {}\n", operand(header));
            mirrorable(header);
        }
        unit += "\n";
        // By the name other files include an entry with, where one has an
        // include path position: <foo.h> by its path would #include_next
        // from the start. Two entries one name finds both come by path.
        llvm::StringMap<std::uint32_t> spelled_by;
        for(auto& entry: interface.entries) {
            spelled_by[operand(entry)] += 1;
        }
        for(auto& entry: interface.entries) {
            auto include = operand(entry);
            unit += std::format(
                "#include {}\n",
                spelled_by[include] > 1 ? std::format("\"{}\"", absolute(entry.file)) : include);
            auto names = mirrorable(entry);
            if(names.empty() && mirrors) {
                result.plan.warnings.push_back(
                    std::format("{}: {} is included by no name a mirror can empty",
                                name,
                                entry.file));
            }
            for(auto& spelled: names) {
                result.files.push_back({std::format("mirror/{}/{}", name, spelled), ""});
            }
        }
        for(auto& entry: roots) {
            module.include_roots.push_back(entry.first().str());
        }
        std::ranges::sort(module.include_roots);

        Exports exports;
        auto scope_of = [](llvm::StringRef qualified) {
            auto separator = qualified.rfind("::");
            return separator == llvm::StringRef::npos
                       ? std::pair{llvm::StringRef(), qualified}
                       : std::pair{qualified.take_front(separator),
                                   qualified.drop_front(separator + 2)};
        };
        for(auto& alias: interface.aliases) {
            auto [scope, alias_name] = scope_of(alias.name);
            exports[scope.str()].first.insert(
                std::format("namespace {} = {};", alias_name.str(), alias.target));
        }
        for(auto& entry: interface.exports) {
            exports[scope_of(entry.name).first.str()].second.insert(
                std::format("using ::{};", entry.name));
        }
        unit += std::format("\nexport module {};\n\n", name) + export_text(exports);
        result.files.push_back({module.source, std::move(unit)});
        result.files.push_back({std::format("{}.macros.h", name), macro_header(interface)});
        result.plan.mirrors.push_back(std::format("mirror/{}", name));
    }

    std::string prelude = "#pragma once\n\n" + base;
    for(auto& name: *order) {
        prelude += std::format("import {};\n", name);
    }
    for(auto& name: *order) {
        prelude += std::format("#include \"{}.macros.h\"\n", name);
    }
    result.plan.prelude = "prelude.h";
    result.files.push_back({result.plan.prelude, std::move(prelude)});
    if(!mirrors) {
        drop_mirrors(result);
    }
    return result;
}

std::expected<Wrapping, std::string> merge(llvm::ArrayRef<Configuration> configurations,
                                           bool mirrors) {
    if(configurations.empty()) {
        return std::unexpected("no configuration to merge");
    }
    Wrapping result;
    llvm::StringSet<> names;
    // A module's unit in each configuration, in their order.
    llvm::StringMap<std::vector<Wrapper>> units;
    std::set<std::string> emptied;
    for(auto [index, configuration]: llvm::enumerate(configurations)) {
        llvm::StringRef name = configuration.name;
        if(name.empty() || name == "mirror" ||
           !llvm::all_of(name, [](char c) { return llvm::isAlnum(c) || c == '_' || c == '-'; }) ||
           !names.insert(name).second) {
            return std::unexpected(
                std::format("configuration {}: not a distinct directory name", configuration.name));
        }
        if(configuration.condition.empty()) {
            return std::unexpected(std::format("configuration {} has no condition", name.str()));
        }
        auto differs = [&] {
            return std::unexpected(std::format("configurations {} and {} wrap different modules",
                                               configurations.front().name,
                                               name.str()));
        };
        for(auto& file: configuration.files) {
            llvm::StringRef path = file.path;
            if(path.starts_with("mirror/")) {
                emptied.insert(file.path);
                continue;
            }
            if(path == "prelude.h" || path.ends_with(".macros.h")) {
                result.files.push_back({std::format("{}/{}", name.str(), file.path), file.content});
                continue;
            }
            if(!path.consume_back(".cppm") || path.contains('/')) {
                return std::unexpected(
                    std::format("configuration {}: {} is no file modularize writes",
                                name.str(),
                                file.path));
            }
            auto unit = read_wrapper(path, file.content);
            if(!unit) {
                return std::unexpected(
                    std::format("configuration {}: {}: {}", name.str(), file.path, unit.error()));
            }
            auto& list = units[path];
            if(list.size() != index) {
                return differs();
            }
            result.files.push_back(
                {std::format("{}/{}.fragment.h", name.str(), path.str()), unit->fragment});
            list.push_back(std::move(*unit));
        }
        if(llvm::any_of(units, [&](auto& entry) { return entry.second.size() != index + 1; })) {
            return differs();
        }
    }

    // Each configuration's file of a name, by the first condition holding.
    auto dispatch = [&](llvm::StringRef file) {
        std::string text;
        for(auto [index, configuration]: llvm::enumerate(configurations)) {
            text += std::format("#{} {}\n#include \"{}/{}\"\n",
                                index == 0 ? "if" : "elif",
                                configuration.condition,
                                configuration.name,
                                file.str());
        }
        return text +
               "#else\n#error \"no configuration merged matches this compilation\"\n#endif\n";
    };
    auto condition = [&](llvm::ArrayRef<std::size_t> group) {
        if(group.size() == 1) {
            return configurations[group.front()].condition;
        }
        llvm::SmallVector<std::string> conditions;
        for(auto index: group) {
            conditions.push_back(std::format("({})", configurations[index].condition));
        }
        return llvm::join(conditions, " || ");
    };

    llvm::StringMap<std::vector<std::string>> imports;
    for(auto& [module, list]: units) {
        auto& imported = imports[module];
        for(auto& unit: list) {
            llvm::SmallVector<llvm::StringRef> lines;
            llvm::StringRef(unit.fragment).split(lines, '\n', -1, false);
            for(auto line: lines) {
                if(line.consume_front("import ") && line.consume_back(";") &&
                   units.contains(line) && !llvm::is_contained(imported, line)) {
                    imported.push_back(line.str());
                }
            }
        }
    }
    auto order = import_order(imports);
    if(!order) {
        return std::unexpected(order.error());
    }

    auto std_mirror =
        llvm::any_of(emptied, [](llvm::StringRef path) { return path.starts_with("mirror/std/"); });
    if(std_mirror) {
        result.plan.mirrors.push_back("mirror/std");
    }
    for(auto& name: *order) {
        auto& module = result.plan.modules.emplace_back();
        module.name = name;
        module.source = std::format("{}.cppm", name);
        module.imports = imports[name];
        if(std_mirror) {
            module.mirrors.push_back("mirror/std");
        }
        for(auto& imported: module.imports) {
            module.mirrors.push_back(std::format("mirror/{}", imported));
        }
        result.plan.mirrors.push_back(std::format("mirror/{}", name));

        // Exported by every configuration first, then by fewer.
        std::map<std::tuple<std::string, bool, std::string>, std::vector<std::size_t>> owners;
        for(auto [index, unit]: llvm::enumerate(units[name])) {
            for(auto& [scope, lines]: unit.exports) {
                for(auto& alias: lines.first) {
                    owners[{scope, true, alias}].push_back(index);
                }
                for(auto& used: lines.second) {
                    owners[{scope, false, used}].push_back(index);
                }
            }
        }
        std::map<std::pair<std::size_t, std::vector<std::size_t>>, Exports> groups;
        for(auto& [key, group]: owners) {
            auto& [scope, alias, line] = key;
            auto& lines = groups[{configurations.size() - group.size(), group}][scope];
            (alias ? lines.first : lines.second).insert(line);
        }
        std::string unit = "module;\n\n" + dispatch(std::format("{}.fragment.h", name)) +
                           std::format("\nexport module {};\n", name);
        for(auto& [key, exports]: groups) {
            unit += "\n";
            if(key.first == 0) {
                unit += export_text(exports);
            } else {
                unit +=
                    std::format("#if {}\n{}#endif\n", condition(key.second), export_text(exports));
            }
        }
        result.files.push_back({module.source, std::move(unit)});
    }
    for(auto& path: emptied) {
        result.files.push_back({path, ""});
    }
    result.plan.prelude = "prelude.h";
    result.files.push_back({result.plan.prelude, "#pragma once\n\n" + dispatch("prelude.h")});
    if(!mirrors) {
        drop_mirrors(result);
    }
    return result;
}

}  // namespace clice::analysis
