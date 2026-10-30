module clice;

import :analysis.wrapping;
import :vfs.file_system;
import :vfs.path;

namespace clice::analysis {

namespace {

struct PartitionFile {
    struct Module {
        std::string name;
        std::vector<std::string> files;

        /// Its headers stay headers beside the wrapped modules.
        std::optional<bool> textual;

        /// An existing module interface stands for it.
        std::optional<bool> external;

        /// The module exporting the names of its headers.
        std::optional<std::string> provides;

        /// A program module rewritten into a named module, and where its
        /// primary interface unit goes.
        std::optional<bool> rewrite;
        std::optional<std::string> primary;
    };

    std::vector<Module> modules;
};

std::expected<llvm::SmallVector<std::string>, std::string> read_lines(llvm::StringRef path) {
    auto text = vfs::read_text(path);
    if(!text) {
        return std::unexpected(
            std::format("cannot read {}: {}", path.str(), text.error().message()));
    }
    llvm::SmallVector<llvm::StringRef> lines;
    llvm::StringRef(*text).split(lines, '\n');
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
        if(ranges::contains(path, name)) {
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
    ranges::sort(names);
    for(auto& name: names) {
        if(auto visited = visit(visit, name); !visited) {
            return std::unexpected(visited.error());
        }
    }
    return order;
}

/// Names an enum without a name by the type of an enumerator, which a
/// using-enum-declaration takes only through an alias.
constexpr llvm::StringLiteral unnamed_enum = "template <class T> using unnamed_enum = T;";

/// What a module exports from one namespace: its aliases, then its names,
/// as no using-declaration exports an alias.
struct ScopeExports {
    std::set<std::string> aliases;
    std::set<std::string> names;
};

/// By enclosing namespace, the global one empty.
using Exports = std::map<std::string, ScopeExports>;

std::string export_text(const Exports& exports) {
    std::string text;
    for(auto& [scope, lines]: exports) {
        if(!text.empty()) {
            text += "\n";
        }
        auto all = llvm::concat<const std::string>(lines.aliases, lines.names);
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
    bool names_unnamed_enums = false;
};

std::expected<Wrapper, std::string> read_wrapper(llvm::StringRef module, llvm::StringRef text) {
    auto declaration = std::format("\nexport module {};\n", module.str());
    auto split = text.find(declaration);
    if(split == llvm::StringRef::npos) {
        return std::unexpected("no module unit modularize writes");
    }
    auto fragment = text.take_front(split);
    if(!fragment.consume_front("module;\n")) {
        return std::unexpected("no module unit modularize writes");
    }
    Wrapper wrapper{.fragment = fragment.trim().str() + "\n"};
    llvm::SmallVector<llvm::StringRef> lines;
    text.drop_front(split + declaration.size()).split(lines, '\n', -1, false);
    std::optional<std::string> scope;
    for(auto line: lines) {
        if(scope && line == "}") {
            scope.reset();
            continue;
        }
        if(!scope && line == unnamed_enum) {
            wrapper.names_unnamed_enums = true;
            continue;
        }
        if(!scope) {
            if(!line.consume_front("export ")) {
                return std::unexpected(std::format("unexpected line: {}", line.str()));
            }
            if(auto opened = line;
               opened.consume_front("namespace ") && opened.consume_back(" {")) {
                scope = opened.str();
                continue;
            }
        }
        auto& [aliases, names] = wrapper.exports[scope.value_or("")];
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
    return wrapper;
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

std::expected<std::optional<StdModules>, std::string> read_std_modules(llvm::StringRef directory) {
    if(directory.empty()) {
        return std::nullopt;
    }
    StdModules result{
        .sources = {path::join(directory, "std.cppm"), path::join(directory, "std.compat.cppm")},
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

    auto compat = path::join(directory, "std.compat");
    auto entries = fs::Directory::open(compat);
    if(!entries) {
        return std::unexpected(
            std::format("cannot list {}: {}", compat, entries.error().message()));
    }
    for(auto& entry: *entries) {
        if(!entry) {
            return std::unexpected(
                std::format("cannot list {}: {}", compat, entry.error().message()));
        }
        if(!entry->name.ends_with(".inc")) {
            continue;
        }
        auto exported = read_lines(path::join(compat, entry->name));
        if(!exported) {
            return std::unexpected(exported.error());
        }
        for(llvm::StringRef line: *exported) {
            if(line.consume_front("using ::")) {
                result.compat.insert(line.take_until([](char c) { return c == ' ' || c == ';'; }));
            }
        }
    }
    if(result.headers.empty() || result.compat.empty() || !fs::is_file(result.sources.back())) {
        return std::unexpected(
            std::format("{} holds no libc++ std and std.compat modules", directory.str()));
    }
    return result;
}

std::expected<PartitionSpec, std::string> read_partition(PartitionSpec spec,
                                                         llvm::StringRef path,
                                                         const std::optional<StdModules>& libcxx) {
    if(path.empty()) {
        return spec;
    }
    auto text = vfs::read_text(path);
    if(!text) {
        return std::unexpected(
            std::format("cannot read {}: {}", path.str(), text.error().message()));
    }
    PartitionFile file;
    if(auto result = json::from_string(*text, file); !result) {
        return std::unexpected(
            std::format("{} is not a partition file: {}", path.str(), result.error().message));
    }
    for(auto& module: file.modules) {
        PartitionSpec::Module declared{
            .name = std::move(module.name),
            .files = std::move(module.files),
        };
        auto textual = module.textual.value_or(false);
        auto external = module.external.value_or(false);
        if(textual && external) {
            return std::unexpected(
                std::format("module {} is both textual and external", declared.name));
        }
        if(module.rewrite.value_or(false)) {
            // A kind beside it is left for the partition to reject.
            if(!textual && !external) {
                declared.kind = ModuleKind::Program;
            }
            declared.rewrite = true;
            declared.primary = module.primary.value_or("");
            llvm::StringRef primary = declared.primary;
            if(path::is_absolute(primary) || path::is_absolute(primary, path::Style::Posix) ||
               ranges::contains(path::components(primary), std::string_view(".."))) {
                return std::unexpected(std::format("module {}: primary {} lies outside the project",
                                                   declared.name,
                                                   primary.str()));
            }
        } else if(module.primary) {
            return std::unexpected(
                std::format("module {} has a primary interface but is not rewritten",
                            declared.name));
        }
        if(textual) {
            declared.kind = ModuleKind::Textual;
        }
        if(external) {
            declared.kind = ModuleKind::External;
        }
        if(module.provides) {
            if(*module.provides != "std.compat" || !libcxx) {
                return std::unexpected(
                    std::format("module {} provided by {}: only std.compat, given --std",
                                declared.name,
                                *module.provides));
            }
            for(auto& name: libcxx->compat) {
                declared.provides.insert(name.getKey());
            }
        }
        spec.modules.push_back(std::move(declared));
    }
    return spec;
}

std::expected<Wrapping, std::string> wrap(const Partition& partition,
                                          llvm::ArrayRef<Interface> interfaces,
                                          const std::optional<StdModules>& libcxx,
                                          llvm::StringRef root,
                                          bool mirrors) {
    // By its name where programs include it by one, else by its path.
    auto operand = [&](const InterfaceHeader& header) {
        return header.include == std::format("\"{}\"", header.file)
                   ? std::format("\"{}\"", path::join(root, header.file))
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
    ranges::sort(given, [&](const Interface* lhs, const Interface* rhs) {
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
    std::map<std::pair<std::string, std::string>, std::map<std::string, std::set<std::string>>>
        shared;
    for(auto& entry: generated) {
        for(auto& local: entry.second->shared_locals) {
            for(auto& other: local.modules) {
                if(entry.first() < other) {
                    shared[{entry.first().str(), other}][local.file].insert(local.name);
                }
            }
        }
    }
    for(auto& [modules, files]: shared) {
        llvm::SmallVector<std::string> parts;
        for(auto& [file, names]: files) {
            parts.push_back(std::format("{} ({})", llvm::join(names, ", "), file));
        }
        result.plan.warnings.push_back(std::format(
            "{} and {} both use TU-local {}: an importer using the code of both can define each "
            "twice under one name; wrap them as one module",
            modules.first,
            modules.second,
            llvm::join(parts, "; ")));
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

    // The C and C++ libraries' headers are system headers, whose macros
    // clang takes without a word where they redefine its own (glibc's
    // __INT64_C, spaced unlike clang's): so are their replays.
    auto macro_header = [](const Interface& interface, bool system) {
        std::string text = "#pragma once\n";
        if(system && !interface.macros.empty()) {
            text += "#pragma clang system_header\n";
        }
        for(auto& macro: interface.macros) {
            text += macro.directive + "\n";
        }
        return text;
    };
    for(auto* module: given) {
        result.files.push_back(
            {std::format("{}.macros.h", module->module), macro_header(*module, true)});
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

    // The names a mirror can shadow: inside it, as no `..` or absolute path
    // is; each also tells the root the header is found under.
    auto mirrorable = [&](const InterfaceHeader& header, llvm::StringSet<>& roots) {
        llvm::SmallVector<std::string> names;
        for(auto& name: header.names) {
            std::string spelled(name);
            path::normalize(spelled, path::DotDot::Fold, path::Style::Posix);
            if(spelled.empty() || spelled.starts_with("../") || spelled.contains('\\') ||
               path::is_absolute(spelled)) {
                continue;
            }
            auto file = path::join(root, header.file);
            if(file.ends_with("/" + spelled)) {
                roots.insert(llvm::StringRef(file).drop_back(spelled.size() + 1));
            }
            names.push_back(std::move(spelled));
        }
        return names;
    };
    auto scope_of = [](llvm::StringRef qualified) {
        auto separator = qualified.rfind("::");
        return separator == llvm::StringRef::npos ? std::pair{llvm::StringRef(), qualified}
                                                  : std::pair{qualified.take_front(separator),
                                                              qualified.drop_front(separator + 2)};
    };
    // Adds the alias declarations and the using-declarations exporting
    // `names`; whether one names an enumeration without a name.
    auto export_names = [&](Exports& exports,
                            llvm::ArrayRef<InterfaceAlias> aliases,
                            llvm::ArrayRef<InterfaceExport> names) {
        for(auto& alias: aliases) {
            auto [scope, alias_name] = scope_of(alias.name);
            exports[scope.str()].aliases.insert(
                std::format("namespace {} = {};", alias_name.str(), alias.target));
        }
        bool unnamed = false;
        for(auto& entry: names) {
            auto& lines = exports[scope_of(entry.name).first.str()].names;
            switch(entry.form) {
                case ExportForm::Name: lines.insert(std::format("using ::{};", entry.name)); break;
                case ExportForm::Enum:
                    lines.insert(std::format("using enum ::{};", entry.name));
                    break;
                case ExportForm::EnumOf:
                    lines.insert(
                        std::format("using enum unnamed_enum<decltype(::{})>;", entry.name));
                    unnamed = true;
                    break;
            }
        }
        return unnamed;
    };
    auto unit_text = [&](std::string unit,
                         llvm::StringRef name,
                         const Exports& exports,
                         bool names_unnamed_enums) {
        unit += std::format("\nexport module {};\n\n", name.str());
        if(names_unnamed_enums) {
            unit += std::format("{}\n\n", unnamed_enum.str());
        }
        return unit + export_text(exports);
    };

    // A kept module's unit exports what files of other modules reach only
    // through its emptied headers, which its fragment includes. Its
    // internal-linkage entities stay with the headers holding them.
    for(auto* module: given) {
        if(partition.kinds[partition.module_named(module->module)] != ModuleKind::Textual) {
            continue;
        }
        auto& entry = result.plan.modules.emplace_back();
        entry.name = module->module;
        entry.source = std::format("{}.cppm", module->module);
        std::string unit = "module;\n\n";
        llvm::StringSet<> roots;
        for(auto& header: module->textual) {
            unit += std::format("#include {}\n", operand(header));
            mirrorable(header, roots);
        }
        for(auto& root: roots) {
            entry.include_roots.push_back(root.first().str());
        }
        ranges::sort(entry.include_roots);
        Exports exports;
        auto unnamed = export_names(exports, module->aliases, module->kept_exports);
        result.files.push_back({entry.source, unit_text(unit, entry.name, exports, unnamed)});
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
        // What the imported modules cannot export that its headers name,
        // which the emptied headers no longer bring in.
        for(auto& header: interface.textual_uses) {
            unit += std::format("#include {}\n", operand(header));
            mirrorable(header, roots);
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
            unit += std::format("#include {}\n",
                                spelled_by[include] > 1
                                    ? std::format("\"{}\"", path::join(root, entry.file))
                                    : include);
            auto names = mirrorable(entry, roots);
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
        ranges::sort(module.include_roots);

        Exports exports;
        auto unnamed = export_names(exports, interface.aliases, interface.exports);
        result.files.push_back({module.source, unit_text(unit, name, exports, unnamed)});
        result.files.push_back({std::format("{}.macros.h", name), macro_header(interface, false)});
        result.plan.mirrors.push_back(std::format("mirror/{}", name));
    }

    // What every macro header replays, and nothing else: a compilation
    // force-includes it, or a file includes it anywhere, even ahead of its
    // module declaration.
    std::string macros = "#pragma once\n\n";
    for(auto* module: given) {
        macros += std::format("#include \"{}.macros.h\"\n", module->module);
    }
    for(auto& name: *order) {
        macros += std::format("#include \"{}.macros.h\"\n", name);
    }
    result.plan.macros = "macros.h";
    result.files.push_back({result.plan.macros, std::move(macros)});

    // A translation unit that is no module unit imports the modules through
    // the prelude, and includes itself the headers no unit can export.
    std::string prelude = "#pragma once\n\n";
    std::set<std::string> local_includes;
    for(auto* module: given) {
        for(auto& local: module->locals) {
            local_includes.insert(operand(local.entry));
        }
    }
    for(auto& include: local_includes) {
        prelude += std::format("#include {}\n", include);
    }
    if(libcxx) {
        prelude += "import std.compat;\n";
    }
    for(auto& module: result.plan.modules) {
        prelude += std::format("import {};\n", module.name);
    }
    prelude += std::format("#include \"{}\"\n", result.plan.macros);
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
    llvm::StringMap<std::vector<Wrapper>> wrappers;
    std::set<std::string> emptied;
    // The mirrors apply to every configuration alike.
    std::optional<bool> std_emptied;
    for(auto [index, configuration]: views::enumerate(configurations)) {
        llvm::StringRef name = configuration.name;
        if(name.empty() || name == "mirror" ||
           !ranges::all_of(name, [](char c) { return llvm::isAlnum(c) || c == '_' || c == '-'; }) ||
           !names.insert(name).second) {
            return std::unexpected(
                std::format("configuration {}: not a distinct directory name", configuration.name));
        }
        if(configuration.condition.empty()) {
            return std::unexpected(std::format("configuration {} has no condition", name.str()));
        }
        auto empties_std = ranges::any_of(configuration.files, [](const Wrapping::File& file) {
            return llvm::StringRef(file.path).starts_with("mirror/std/");
        });
        if(mirrors && std_emptied && *std_emptied != empties_std) {
            return std::unexpected(
                std::format("configurations {} and {} differ in emptying the standard headers",
                            configurations.front().name,
                            name.str()));
        }
        std_emptied = empties_std;
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
            if(path == "prelude.h" || path == "macros.h" || path.ends_with(".macros.h")) {
                result.files.push_back({std::format("{}/{}", name.str(), file.path), file.content});
                continue;
            }
            if(!path.consume_back(".cppm") || path.contains('/')) {
                return std::unexpected(
                    std::format("configuration {}: {} is no file modularize writes",
                                name.str(),
                                file.path));
            }
            auto wrapper = read_wrapper(path, file.content);
            if(!wrapper) {
                return std::unexpected(std::format("configuration {}: {}: {}",
                                                   name.str(),
                                                   file.path,
                                                   wrapper.error()));
            }
            auto& list = wrappers[path];
            if(list.size() != static_cast<std::size_t>(index)) {
                return differs();
            }
            result.files.push_back(
                {std::format("{}/{}.fragment.h", name.str(), path.str()), wrapper->fragment});
            list.push_back(std::move(*wrapper));
        }
        if(ranges::any_of(wrappers, [&](auto& entry) {
               return entry.second.size() != static_cast<std::size_t>(index) + 1;
           })) {
            return differs();
        }
    }

    // Each configuration's file of a name, by the first condition holding.
    auto dispatch = [&](llvm::StringRef file) {
        std::string text;
        for(auto [index, configuration]: views::enumerate(configurations)) {
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
    for(auto& [module, list]: wrappers) {
        auto& imported = imports[module];
        for(auto& wrapper: list) {
            llvm::SmallVector<llvm::StringRef> lines;
            llvm::StringRef(wrapper.fragment).split(lines, '\n', -1, false);
            for(auto line: lines) {
                if(line.consume_front("import ") && line.consume_back(";") &&
                   wrappers.contains(line) && !ranges::contains(imported, line)) {
                    imported.push_back(line.str());
                }
            }
        }
    }
    auto order = import_order(imports);
    if(!order) {
        return std::unexpected(order.error());
    }

    auto std_mirror = ranges::any_of(emptied, [](llvm::StringRef path) {
        return path.starts_with("mirror/std/");
    });
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
        // A kept module's unit empties none of its headers.
        auto mirror = std::format("mirror/{}/", name);
        if(ranges::any_of(emptied,
                          [&](llvm::StringRef path) { return path.starts_with(mirror); })) {
            result.plan.mirrors.push_back(mirror.substr(0, mirror.size() - 1));
        }

        // Exported by every configuration first, then by fewer.
        std::map<std::tuple<std::string, bool, std::string>, std::vector<std::size_t>> owners;
        for(auto [index, wrapper]: views::enumerate(wrappers[name])) {
            for(auto& [scope, lines]: wrapper.exports) {
                for(auto& alias: lines.aliases) {
                    owners[{scope, true, alias}].push_back(static_cast<std::size_t>(index));
                }
                for(auto& used: lines.names) {
                    owners[{scope, false, used}].push_back(static_cast<std::size_t>(index));
                }
            }
        }
        std::map<std::pair<std::size_t, std::vector<std::size_t>>, Exports> groups;
        for(auto& [key, group]: owners) {
            auto& [scope, alias, line] = key;
            auto& lines = groups[{configurations.size() - group.size(), group}][scope];
            (alias ? lines.aliases : lines.names).insert(line);
        }
        std::string unit = "module;\n\n" + dispatch(std::format("{}.fragment.h", name)) +
                           std::format("\nexport module {};\n", name);
        if(ranges::any_of(wrappers[name], &Wrapper::names_unnamed_enums)) {
            unit += std::format("\n{}\n", unnamed_enum.str());
        }
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
    result.plan.macros = "macros.h";
    result.files.push_back({result.plan.macros, "#pragma once\n\n" + dispatch("macros.h")});
    result.plan.prelude = "prelude.h";
    result.files.push_back({result.plan.prelude, "#pragma once\n\n" + dispatch("prelude.h")});
    if(!mirrors) {
        drop_mirrors(result);
    }
    return result;
}

}  // namespace clice::analysis
