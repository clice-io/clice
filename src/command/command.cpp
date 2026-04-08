#include "command/command.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <ranges>
#include <string_view>

#include "simdjson.h"
#include "command/toolchain.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StringSaver.h"

namespace clice {

namespace {

namespace ranges = std::ranges;

}  // namespace

CompilationDatabase::CompilationDatabase() = default;

CompilationDatabase::~CompilationDatabase() = default;

llvm::ArrayRef<CompilationEntry> CompilationDatabase::find_entries(std::uint32_t path_id) const {
    auto [first, last] = ranges::equal_range(entries, path_id, {}, &CompilationEntry::file);
    if(first == last)
        return {};
    return {&*first, static_cast<size_t>(last - first)};
}

namespace {

/// Shared render logic for a parsed argument. Calls `emit(StringRef)` for each
/// output token, handling all four render styles.
template <typename Emit>
void render_arg_to(Emit&& emit, llvm::opt::Arg& arg) {
    switch(arg.getOption().getRenderStyle()) {
        case llvm::opt::Option::RenderValuesStyle:
            for(auto value: arg.getValues()) {
                emit(llvm::StringRef(value));
            }
            break;

        case llvm::opt::Option::RenderSeparateStyle:
            emit(arg.getSpelling());
            for(auto value: arg.getValues()) {
                emit(llvm::StringRef(value));
            }
            break;

        case llvm::opt::Option::RenderJoinedStyle: {
            llvm::SmallString<256> first = {arg.getSpelling(), arg.getValue(0)};
            emit(llvm::StringRef(first));
            for(auto value: llvm::ArrayRef(arg.getValues()).drop_front()) {
                emit(llvm::StringRef(value));
            }
            break;
        }

        case llvm::opt::Option::RenderCommaJoinedStyle: {
            llvm::SmallString<256> buffer = arg.getSpelling();
            for(unsigned i = 0; i < arg.getNumValues(); i++) {
                if(i)
                    buffer += ',';
                buffer += arg.getValue(i);
            }
            emit(llvm::StringRef(buffer));
            break;
        }
    }
}

}  // namespace

llvm::ArrayRef<const char*> CompilationDatabase::persist_args(llvm::ArrayRef<const char*> args) {
    if(args.empty())
        return {};
    auto* buf = allocator->Allocate<const char*>(args.size());
    std::ranges::copy(args, buf);
    return {buf, args.size()};
}

object_ptr<CompilationInfo>
    CompilationDatabase::save_compilation_info(llvm::StringRef file,
                                               llvm::StringRef directory,
                                               llvm::ArrayRef<const char*> arguments) {
    assert(!arguments.empty() && "arguments must contain at least the driver");

    auto render_arg = [&](auto& out, llvm::opt::Arg& arg) {
        render_arg_to([&](llvm::StringRef s) { out.push_back(strings.save(s).data()); }, arg);
    };

    llvm::SmallVector<const char*, 32> canonical_args;
    llvm::SmallVector<const char*, 16> patch_args;

    /// Driver goes into canonical.
    canonical_args.push_back(strings.save(arguments[0]).data());

    bool remove_pch = false;

    parser->set_visibility(default_visibility(arguments[0]));

    auto on_error = [&](int index, int count) {
        LOG_WARN("missing argument index: {}, count: {} when parse: {}", index, count, file);
    };

    parser->parse(
        llvm::ArrayRef(arguments).drop_front(),
        [&](std::unique_ptr<llvm::opt::Arg> arg) {
            auto& opt = arg->getOption();
            auto id = opt.getID();

            /// Discard options irrelevant to frontend.
            if(is_discarded_option(id)) {
                return;
            }

            /// Discard codegen-only options.
            if(is_codegen_option(id, opt)) {
                return;
            }

            /// Handle CMake's Xclang PCH workaround:
            /// -Xclang -include-pch -Xclang <pchfile> → discard both pairs.
            if(is_xclang_option(id) && arg->getNumValues() == 1) {
                if(remove_pch) {
                    remove_pch = false;
                    return;
                }
                llvm::StringRef value = arg->getValue(0);
                if(value == "-include-pch") {
                    remove_pch = true;
                    return;
                }
            }

            /// User-content options go into per-file patch.
            if(is_user_content_option(id)) {
                /// Absolutize relative paths for include-path options.
                if(is_include_path_option(id) && arg->getNumValues() == 1) {
                    patch_args.push_back(strings.save(arg->getSpelling()).data());
                    llvm::StringRef value = arg->getValue(0);
                    if(!value.empty() && !path::is_absolute(value)) {
                        patch_args.push_back(strings.save(path::join(directory, value)).data());
                    } else {
                        patch_args.push_back(strings.save(value).data());
                    }
                    return;
                }
                render_arg(patch_args, *arg);
                return;
            }

            /// Everything else goes into canonical.
            render_arg(canonical_args, *arg);
        },
        on_error);

    /// Dedup canonical command.
    auto canonical_id = canonicals.get(CanonicalCommand{canonical_args});
    auto canonical = canonicals.get(canonical_id);
    if(canonical->arguments.data() == canonical_args.data()) {
        canonical->arguments = persist_args(canonical_args);
    }

    /// Build and dedup CompilationInfo.
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
    // Clear old entries and caches (but keep allocator/strings/canonicals/infos/toolchain).
    entries.clear();
    search_config_cache.clear();

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

        // Skip non-C-family files (e.g. .rc, .asm, .def) that some build
        // systems emit into compile_commands.json.
        if(!is_c_family_file(file_ref)) {
            ++index;
            continue;
        }

        // Resolve relative file paths against the directory so that entries
        // from different directories don't collide in the PathPool.
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

    // Sort by file path_id for binary search.
    ranges::sort(entries, {}, &CompilationEntry::file);

    return entries.size();
}

llvm::SmallVector<CompileCommand> CompilationDatabase::lookup(llvm::StringRef file,
                                                              const CommandOptions& options) {
    auto path_id = paths.intern(file);
    auto matched = find_entries(path_id);

    auto render_arg = [&](auto& out, llvm::opt::Arg& arg) {
        render_arg_to([&](llvm::StringRef s) { out.push_back(strings.save(s).data()); }, arg);
    };

    /// Build one CompileCommand from a single CompilationInfo.
    auto build_command = [&](object_ptr<CompilationInfo> info) -> CompileCommand {
        llvm::StringRef directory = info->directory;
        std::vector<const char*> flags;
        bool is_cc1 = false;

        auto append_flag = [&](llvm::StringRef s) {
            flags.emplace_back(strings.save(s).data());
        };

        auto append_flags = [&](llvm::ArrayRef<const char*> args) {
            flags.insert(flags.end(), args.begin(), args.end());
        };

        if(options.query_toolchain) {
            auto cached = query_toolchain_cached(file, directory, info->canonical->arguments);

            if(cached.empty()) {
                if(!options.suppress_logging) {
                    LOG_WARN("failed to query toolchain: {}", file);
                }
                append_flags(info->canonical->arguments);
                append_flags(info->patch);
            } else {
                flags.assign(cached.begin(), cached.end());
                // Remove temp source file (last arg from toolchain query).
                flags.pop_back();

                // Replace resource dir if needed.
                if(!resource_dir().empty()) {
                    llvm::StringRef old_resource_dir;
                    for(std::size_t i = 0; i + 1 < flags.size(); ++i) {
                        if(flags[i] == llvm::StringRef("-resource-dir")) {
                            old_resource_dir = flags[i + 1];
                            break;
                        }
                    }
                    if(!old_resource_dir.empty() && old_resource_dir != resource_dir()) {
                        for(auto& flag: flags) {
                            llvm::StringRef s(flag);
                            if(s.starts_with(old_resource_dir)) {
                                auto replaced =
                                    resource_dir().str() + s.substr(old_resource_dir.size()).str();
                                flag = strings.save(replaced).data();
                            }
                        }
                    }
                }

                append_flags(info->patch);

                // Strip -main-file-name and its value — file identity is handled
                // by CompileCommand::to_argv() at materialization time.
                std::vector<const char*> cleaned;
                cleaned.reserve(flags.size());
                for(std::size_t i = 0; i < flags.size(); ++i) {
                    if(flags[i] == llvm::StringRef("-main-file-name") && i + 1 < flags.size()) {
                        ++i;  // skip the value too
                        continue;
                    }
                    cleaned.push_back(flags[i]);
                }
                flags = std::move(cleaned);

                is_cc1 = (flags.size() >= 2 && flags[1] == llvm::StringRef("-cc1"));
            }

            // Inject our resource dir if not already present.
            if(!resource_dir().empty()) {
                bool has_resource_dir = false;
                for(auto& flag: flags) {
                    if(flag == llvm::StringRef("-resource-dir")) {
                        has_resource_dir = true;
                        break;
                    }
                }
                if(!has_resource_dir) {
                    append_flag("-resource-dir");
                    append_flag(resource_dir());
                }
            }
        } else {
            append_flags(info->canonical->arguments);
            append_flags(info->patch);
        }

        // Apply remove filter.
        if(!options.remove.empty()) {
            using Arg = std::unique_ptr<llvm::opt::Arg>;
            llvm::SmallVector<const char*> remove_strs;
            for(auto& s: options.remove) {
                remove_strs.push_back(strings.save(s).data());
            }
            llvm::SmallVector<Arg> remove_args;
            parser->parse(
                remove_strs,
                [&remove_args](Arg arg) { remove_args.emplace_back(std::move(arg)); },
                [](int, int) {});
            auto get_id = [](const Arg& arg) {
                return arg->getOption().getID();
            };
            std::ranges::sort(remove_args, {}, get_id);

            auto saved_flags = std::move(flags);
            flags.clear();
            flags.push_back(saved_flags.front());

            parser->parse(
                llvm::ArrayRef(saved_flags).drop_front(),
                [&](Arg arg) {
                    auto id = arg->getOption().getID();
                    auto range = std::ranges::equal_range(remove_args, id, {}, get_id);
                    for(auto& remove: range) {
                        if(remove->getNumValues() == 1 &&
                           remove->getValue(0) == llvm::StringRef("*")) {
                            return;
                        }
                        if(std::ranges::equal(
                               arg->getValues(),
                               remove->getValues(),
                               [](llvm::StringRef l, llvm::StringRef r) { return l == r; })) {
                            return;
                        }
                    }
                    render_arg(flags, *arg);
                },
                [](int, int) {});
        }

        for(auto& arg: options.append) {
            append_flag(arg);
        }

        return CompileCommand{
            ResolvedFlags{directory, std::move(flags), is_cc1},
            paths.resolve(path_id).data(),
        };
    };

    llvm::SmallVector<CompileCommand> results;

    if(!matched.empty()) {
        for(auto& entry: matched) {
            results.push_back(build_command(entry.info));
        }
    } else {
        // No matching entry — synthesize a default command.
        std::vector<const char*> flags;
        if(file.ends_with(".cpp") || file.ends_with(".hpp") || file.ends_with(".cc")) {
            flags = {"clang++", "-std=c++20"};
        } else {
            flags = {"clang"};
        }
        results.push_back(CompileCommand{
            ResolvedFlags{{}, std::move(flags), false},
            paths.resolve(path_id).data(),
        });
    }

    return results;
}

SearchConfig CompilationDatabase::lookup_search_config(llvm::StringRef file,
                                                       const CommandOptions& options) {
    auto path_id = paths.intern(file);
    auto matched = find_entries(path_id);

    // Only cache when remove/append are empty — custom options produce
    // per-call results that shouldn't pollute the shared cache.
    bool cacheable = !matched.empty() && options.remove.empty() && options.append.empty();

    if(cacheable) {
        auto key = ConfigCacheKey{matched.front().info.ptr, options_bits(options)};
        auto cache_it = search_config_cache.find(key);
        if(cache_it != search_config_cache.end()) {
            return cache_it->second;
        }
    }

    auto results = lookup(file, options);
    auto& cmd = results.front();
    auto config = extract_search_config(cmd.resolved.flags, cmd.resolved.directory);

    if(cacheable) {
        auto key = ConfigCacheKey{matched.front().info.ptr, options_bits(options)};
        search_config_cache.try_emplace(key, config);
    }
    return config;
}

bool CompilationDatabase::has_cached_configs() const {
    return !search_config_cache.empty();
}

CompilationDatabase::ToolchainExtract
    CompilationDatabase::extract_toolchain_flags(llvm::StringRef file,
                                                 llvm::ArrayRef<const char*> arguments) {
    ToolchainExtract result;

    // Driver binary (first arg) — e.g. "clang++" vs "clang" affects language mode.
    result.key += arguments[0];
    result.key += '\0';

    // File extension affects language mode (C vs C++).
    result.key += path::extension(file);
    result.key += '\0';

    result.query_args.push_back(arguments[0]);

    parser->set_visibility(default_visibility(arguments[0]));

    parser->parse(
        llvm::ArrayRef(arguments).drop_front(),
        [&](std::unique_ptr<llvm::opt::Arg> arg) {
            auto id = arg->getOption().getID();
            if(!is_toolchain_option(id)) {
                return;
            }

            // Add option ID and all its values to the cache key.
            result.key += std::to_string(id);
            result.key += '\0';
            for(auto value: arg->getValues()) {
                result.key += value;
                result.key += '\0';
            }

            // Render the argument back to query args.
            render_arg_to(
                [&](llvm::StringRef s) { result.query_args.push_back(strings.save(s).data()); },
                *arg);
        },
        [](int, int) {});

    return result;
}

llvm::ArrayRef<const char*>
    CompilationDatabase::query_toolchain_cached(llvm::StringRef file,
                                                llvm::StringRef directory,
                                                llvm::ArrayRef<const char*> arguments) {
    auto [key, query_args] = extract_toolchain_flags(file, arguments);
    auto it = toolchain_cache.find(key);
    if(it != toolchain_cache.end()) {
        return it->second;
    }

    LOG_WARN("Toolchain cache miss (spawning process): file={}, cache_size={}, key_len={}",
             file,
             toolchain_cache.size(),
             key.size());

    auto callback = [&](const char* s) -> const char* {
        return strings.save(s).data();
    };
    toolchain::QueryParams params = {file, directory, query_args, callback};
    auto result = toolchain::query_toolchain(params);

    auto [entry, _] = toolchain_cache.try_emplace(std::move(key), std::move(result));
    return entry->second;
}

std::vector<ToolchainQuery>
    CompilationDatabase::get_pending_queries(llvm::ArrayRef<PendingEntry> entries) {
    llvm::StringMap<bool> seen_keys;
    std::vector<ToolchainQuery> queries;

    for(auto& entry: entries) {
        if(entry.arguments.empty()) {
            continue;
        }

        auto [key, query_args] = extract_toolchain_flags(entry.file, entry.arguments);

        // Skip if already cached or already queued.
        if(toolchain_cache.count(key) || !seen_keys.try_emplace(key, true).second) {
            continue;
        }

        LOG_DEBUG("Pre-warm: new toolchain key (len={}) for file={}", key.size(), entry.file);
        queries.push_back(
            {std::move(key), std::move(query_args), entry.file.str(), entry.directory.str()});
    }

    LOG_INFO("Pre-warm: {} unique keys from {} entries, {} queries needed",
             seen_keys.size(),
             entries.size(),
             queries.size());
    return queries;
}

void CompilationDatabase::inject_results(llvm::ArrayRef<ToolchainResult> results) {
    for(auto& result: results) {
        if(toolchain_cache.count(result.key)) {
            continue;
        }
        std::vector<const char*> saved;
        saved.reserve(result.cc1_args.size());
        for(auto& arg: result.cc1_args) {
            saved.push_back(strings.save(arg).data());
        }
        toolchain_cache.try_emplace(result.key, std::move(saved));
    }
}

bool CompilationDatabase::has_cached_toolchain() const {
    return !toolchain_cache.empty();
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

#ifdef CLICE_ENABLE_TEST

void CompilationDatabase::add_command(llvm::StringRef directory,
                                      llvm::StringRef file,
                                      llvm::ArrayRef<const char*> arguments) {
    auto path_id = paths.intern(file);
    auto info = save_compilation_info(file, directory, arguments);
    // Insert in sorted position to maintain sort invariant.
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
