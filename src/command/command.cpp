#include "command/command.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <ranges>
#include <string_view>

#include "simdjson.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StringSaver.h"

namespace clice {

namespace {

namespace ranges = std::ranges;

}  // namespace

std::vector<const char*> CompileCommand::to_argv() const {
    std::vector<const char*> argv;
    argv.reserve(resolved.flags.size() + 4);

    if(resolved.is_cc1 && source_file) {
        for(std::size_t i = 0; i < resolved.flags.size(); ++i) {
            argv.push_back(resolved.flags[i]);
            if(resolved.flags[i] == llvm::StringRef("-cc1")) {
                argv.push_back("-main-file-name");
                argv.push_back(path::filename(source_file).data());
            }
        }
    } else {
        argv.insert(argv.end(), resolved.flags.begin(), resolved.flags.end());
    }

    if(source_file) {
        argv.push_back(source_file);
    }
    return argv;
}

std::vector<std::string> CompileCommand::to_string_argv() const {
    auto argv = to_argv();
    std::vector<std::string> result;
    result.reserve(argv.size());
    for(auto* arg: argv) {
        result.emplace_back(arg);
    }
    return result;
}

CompilationDatabase::CompilationDatabase() = default;

CompilationDatabase::~CompilationDatabase() = default;

llvm::ArrayRef<CompilationEntry> CompilationDatabase::find_entries(std::uint32_t path_id) const {
    auto [first, last] = ranges::equal_range(entries, path_id, {}, &CompilationEntry::file);
    if(first == last)
        return {};
    return {&*first, static_cast<size_t>(last - first)};
}

llvm::ArrayRef<const char*> CompilationDatabase::persist_args(llvm::ArrayRef<const char*> args) {
    if(args.empty())
        return {};
    auto* buf = allocator->Allocate<const char*>(args.size());
    ranges::copy(args, buf);
    return {buf, args.size()};
}

object_ptr<CompilationInfo>
    CompilationDatabase::save_compilation_info(llvm::StringRef file,
                                               llvm::StringRef directory,
                                               llvm::ArrayRef<const char*> arguments) {
    assert(!arguments.empty() && "arguments must contain at least the driver");

    auto render_arg = [&](auto& out, const kota::option::ParsedArg& arg) {
        auto cb = [&](std::string_view s) {
            out.push_back(strings.save(s).data());
        };
        option::table().render(arg, cb);
    };

    llvm::SmallVector<const char*, 32> canonical_args;
    llvm::SmallVector<const char*, 16> patch_args;

    canonical_args.push_back(strings.save(arguments[0]).data());

    bool remove_pch = false;

    std::vector<std::string> parse_args(arguments.begin() + 1, arguments.end());
    auto options = kota::option::ParseOptions{.dash_dash_parsing = true,
                                              .visibility = default_visibility(arguments[0])};
    for(auto& result: option::table().parse(parse_args, options)) {
        if(!result.has_value()) {
            auto& err = result.error();
            LOG_WARN("parse error at index {}: {} when parse: {}", err.index, err.message, file);
            continue;
        }
        auto& arg = *result;
        auto id = arg.id;

        /// Discard options irrelevant to frontend.
        if(is_discarded_option(id)) {
            continue;
        }

        /// Discard codegen-only options.
        if(is_codegen_option(id)) {
            continue;
        }

        /// Handle CMake's Xclang PCH workaround:
        /// -Xclang -include-pch -Xclang <pchfile> → discard both pairs.
        if(is_xclang_option(id) && arg.values.size() == 1) {
            if(remove_pch) {
                remove_pch = false;
                continue;
            }
            std::string_view value = arg.values[0];
            if(value == "-include-pch") {
                remove_pch = true;
                continue;
            }
        }

        /// User-content options go into per-file patch.
        if(is_user_content_option(id)) {
            /// Absolutize relative paths for include-path options.
            if(is_include_path_option(id) && arg.values.size() == 1) {
                patch_args.push_back(
                    strings.save(option::table().option(id)->prefixed_name()).data());
                llvm::StringRef value(arg.values[0]);
                if(!value.empty() && !path::is_absolute(value)) {
                    patch_args.push_back(strings.save(path::join(directory, value)).data());
                } else {
                    patch_args.push_back(strings.save(value).data());
                }
                continue;
            }
            render_arg(patch_args, arg);
            continue;
        }

        /// Everything else goes into canonical.
        render_arg(canonical_args, arg);
    }

    auto canonical_id = canonicals.get(CanonicalCommand{canonical_args});
    auto canonical = canonicals.get(canonical_id);
    if(canonical->arguments.data() == canonical_args.data()) {
        canonical->arguments = persist_args(canonical_args);
    }

    auto dir = strings.save(directory).data();
    auto info_id = infos.get(CompilationInfo{dir, canonical, patch_args});
    auto info = infos.get(info_id);
    if(info->patch.data() == patch_args.data()) {
        info->patch = persist_args(patch_args);
    }

    return info;
}

object_ptr<CompilationInfo> CompilationDatabase::save_compilation_info(llvm::StringRef file,
                                                                       llvm::StringRef directory,
                                                                       llvm::StringRef command) {
    llvm::BumpPtrAllocator local;
    llvm::StringSaver saver(local);

    llvm::SmallVector<const char*, 32> arguments;

#ifdef _WIN32
    llvm::cl::TokenizeWindowsCommandLineFull(command, saver, arguments);
#else
    llvm::cl::TokenizeGNUCommandLine(command, saver, arguments);
#endif

    if(arguments.empty()) {
        return {nullptr};
    }

    return save_compilation_info(file, directory, arguments);
}

std::size_t CompilationDatabase::load(llvm::StringRef path) {
    entries.clear();

    simdjson::padded_string json_buf;
    if(auto error = simdjson::padded_string::load(std::string(path)).get(json_buf)) {
        LOG_ERROR("Failed to read compilation database from {}: {}",
                  path,
                  simdjson::error_message(error));
        return 0;
    }

    simdjson::ondemand::parser json_parser;
    simdjson::ondemand::document doc;
    if(auto error = json_parser.iterate(json_buf).get(doc)) {
        LOG_ERROR("Failed to parse compilation database from {}: {}",
                  path,
                  simdjson::error_message(error));
        return 0;
    }

    simdjson::ondemand::array arr;
    if(auto error = doc.get_array().get(arr)) {
        LOG_ERROR("Invalid compilation database format in {}: root element must be an array.",
                  path);
        return 0;
    }

    std::size_t index = 0;
    for(auto element: arr) {
        simdjson::ondemand::object obj;
        if(element.get_object().get(obj)) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}: " "item is not an object.",
                path,
                index);
            ++index;
            continue;
        }

        std::string_view dir_sv, file_sv;
        if(obj["directory"].get_string().get(dir_sv)) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}: " "'directory' key is missing.",
                path,
                index);
            ++index;
            continue;
        }

        if(obj["file"].get_string().get(file_sv)) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}: " "'file' key is missing.",
                path,
                index);
            ++index;
            continue;
        }

        llvm::StringRef dir_ref(dir_sv.data(), dir_sv.size());
        llvm::StringRef file_ref(file_sv.data(), file_sv.size());

        if(!is_c_family_file(file_ref)) {
            ++index;
            continue;
        }

        std::string file_abs;
        if(!path::is_absolute(file_ref)) {
            file_abs = path::join(dir_ref, file_ref);
            file_ref = file_abs;
        }

        simdjson::ondemand::array args_arr;
        if(!obj["arguments"].get_array().get(args_arr)) {
            llvm::BumpPtrAllocator local;
            llvm::StringSaver saver(local);
            llvm::SmallVector<const char*, 32> args;
            bool malformed = false;
            for(auto arg_val: args_arr) {
                std::string_view sv;
                if(arg_val.get_string().get(sv)) {
                    malformed = true;
                    break;
                }
                args.push_back(saver.save(llvm::StringRef(sv.data(), sv.size())).data());
            }
            if(!malformed && !args.empty()) {
                auto info = save_compilation_info(file_ref, dir_ref, args);
                assert(info && "save_compilation_info must succeed with non-empty args");
                auto path_id = paths.intern(file_ref);
                entries.push_back({path_id, info});
            }
        } else {
            std::string_view cmd_sv;
            if(obj["command"].get_string().get(cmd_sv)) {
                LOG_ERROR(
                    "Invalid compilation database in {}. Skipping item at index {}: " "neither 'arguments' nor 'command' key is present.",
                    path,
                    index);
                ++index;
                continue;
            }
            auto info = save_compilation_info(file_ref,
                                              dir_ref,
                                              llvm::StringRef(cmd_sv.data(), cmd_sv.size()));
            if(!info) {
                ++index;
                continue;
            }
            auto path_id = paths.intern(file_ref);
            entries.push_back({path_id, info});
        }

        ++index;
    }

    ranges::sort(entries, {}, &CompilationEntry::file);

    return entries.size();
}

CompileCommand CompilationDatabase::build_command(std::uint32_t path_id,
                                                  object_ptr<CompilationInfo> info,
                                                  const CommandOptions& options) {
    auto render_arg = [&](auto& out, const kota::option::ParsedArg& arg) {
        auto cb = [&](std::string_view s) {
            out.push_back(strings.save(s).data());
        };
        option::table().render(arg, cb);
    };

    llvm::StringRef directory = info->directory;
    std::vector<const char*> flags;

    auto append_arg = [&](llvm::StringRef s) {
        flags.emplace_back(strings.save(s).data());
    };

    auto append_args = [&](llvm::ArrayRef<const char*> args) {
        flags.insert(flags.end(), args.begin(), args.end());
    };

    append_args(info->canonical->arguments);
    append_args(info->patch);

    if(options.inject_resource_dir && !resource_dir().empty() &&
       !ranges::contains(flags, llvm::StringRef("-resource-dir"))) {
        append_arg("-resource-dir");
        append_arg(resource_dir());
    }

    if(!options.remove.empty()) {
        std::vector<std::string> remove_strs;
        for(auto& s: options.remove) {
            remove_strs.push_back(s);
        }
        std::vector<kota::option::ParsedArg> remove_args;
        for(auto& result: option::table().parse(remove_strs)) {
            if(result.has_value()) {
                remove_args.push_back(*result);
            }
        }
        auto get_id = [](const kota::option::ParsedArg& arg) {
            return arg.id;
        };
        ranges::sort(remove_args, {}, get_id);

        auto saved_flags = std::move(flags);
        flags.clear();
        flags.push_back(saved_flags.front());

        std::vector<std::string> saved_parse_args(saved_flags.begin() + 1, saved_flags.end());
        for(auto& result: option::table().parse(saved_parse_args)) {
            if(!result.has_value()) {
                continue;
            }
            auto& arg = *result;
            auto id = arg.id;
            auto range = ranges::equal_range(remove_args, id, {}, get_id);
            bool removed = false;
            for(auto& remove: range) {
                if(remove.values.size() == 1 && remove.values[0] == "*") {
                    removed = true;
                    break;
                }
                if(ranges::equal(arg.values, remove.values)) {
                    removed = true;
                    break;
                }
            }
            if(!removed) {
                render_arg(flags, arg);
            }
        }
    }

    for(auto& arg: options.append) {
        append_arg(arg);
    }

    return CompileCommand{
        ResolvedFlags{directory, std::move(flags), false},
        paths.resolve(path_id).data()
    };
}

llvm::SmallVector<CompileCommand> CompilationDatabase::lookup(llvm::StringRef file,
                                                              const CommandOptions& options) {
    auto path_id = paths.intern(file);
    auto matched = find_entries(path_id);

    llvm::SmallVector<CompileCommand> results;

    if(!matched.empty()) {
        for(auto& entry: matched) {
            results.push_back(build_command(path_id, entry.info, options));
        }
    } else {
        std::vector<const char*> flags;
        if(file.ends_with(".cpp") || file.ends_with(".hpp") || file.ends_with(".cc")) {
            flags = {"clang++", "-std=c++20"};
        } else {
            flags = {"clang"};
        }
        if(options.inject_resource_dir && !resource_dir().empty()) {
            flags.push_back(strings.save("-resource-dir").data());
            flags.push_back(strings.save(resource_dir()).data());
        }
        results.push_back(CompileCommand{
            ResolvedFlags{{}, std::move(flags), false},
            paths.resolve(path_id).data()
        });
    }

    return results;
}

llvm::StringRef CompilationDatabase::resolve_path(std::uint32_t path_id) {
    return paths.resolve(path_id);
}

std::uint32_t CompilationDatabase::intern_path(llvm::StringRef path) {
    return paths.intern(path);
}

bool CompilationDatabase::has_entry(llvm::StringRef file) {
    auto path_id = paths.intern(file);
    return !find_entries(path_id).empty();
}

llvm::ArrayRef<CompilationEntry> CompilationDatabase::get_entries() const {
    return entries;
}

llvm::SmallVector<CompilationDatabase::ConfigGroup>
    CompilationDatabase::unique_configs(const CommandOptions& options) {
    // Group entries by CompilationInfo pointer — entries with the same pointer
    // share identical (directory, canonical, patch) and thus identical flags.
    llvm::DenseMap<const CompilationInfo*, std::size_t> group_indices;
    llvm::SmallVector<ConfigGroup> result;
    result.reserve(entries.size());

    for(auto& entry: entries) {
        auto [it, inserted] = group_indices.try_emplace(entry.info.ptr, result.size());
        if(inserted) {
            result.push_back({{}, build_command(entry.file, entry.info, options), entry.info});
        }

        auto& file_ids = result[it->second].file_ids;
        if(file_ids.empty() || file_ids.back() != entry.file) {
            file_ids.push_back(entry.file);
        }
    }

    return result;
}

CompileCommand CompilationDatabase::group_command(const ConfigGroup& group,
                                                  const CommandOptions& options) {
    assert(!group.file_ids.empty() && group.info && "group must come from unique_configs()");
    return build_command(group.file_ids.front(), group.info, options);
}

#ifdef CLICE_ENABLE_TEST

void CompilationDatabase::add_command(llvm::StringRef directory,
                                      llvm::StringRef file,
                                      llvm::ArrayRef<const char*> arguments) {
    auto path_id = paths.intern(file);
    auto info = save_compilation_info(file, directory, arguments);
    auto it = ranges::lower_bound(entries, path_id, {}, &CompilationEntry::file);
    entries.insert(it, {path_id, info});
}

void CompilationDatabase::add_command(llvm::StringRef directory,
                                      llvm::StringRef file,
                                      llvm::StringRef command) {
    auto path_id = paths.intern(file);
    auto info = save_compilation_info(file, directory, command);
    auto it = ranges::lower_bound(entries, path_id, {}, &CompilationEntry::file);
    entries.insert(it, {path_id, info});
}

#endif

}  // namespace clice
