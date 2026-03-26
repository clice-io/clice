#include "command/command.h"

#include <algorithm>
#include <array>
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

object_ptr<CompilationInfo> CompilationDatabase::find_info(std::uint32_t path_id) const {
    auto it = ranges::lower_bound(entries, path_id, {}, &CompilationEntry::file);
    if(it != entries.end() && it->file == path_id) {
        return it->info;
    }
    return nullptr;
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

void CompilationDatabase::render_arg(llvm::SmallVectorImpl<StringID>& out, llvm::opt::Arg& arg) {
    render_arg_to([&](llvm::StringRef s) { out.push_back(strings.get(s)); }, arg);
}

void CompilationDatabase::render_arg_chars(std::vector<const char*>& out, llvm::opt::Arg& arg) {
    render_arg_to([&](llvm::StringRef s) { out.push_back(strings.save(s).data()); }, arg);
}

llvm::ArrayRef<StringID> CompilationDatabase::persist_ids(llvm::ArrayRef<StringID> ids) {
    if(ids.empty())
        return {};
    auto* buf = allocator->Allocate<StringID>(ids.size());
    std::ranges::copy(ids, buf);
    return {buf, ids.size()};
}

object_ptr<CompilationInfo>
    CompilationDatabase::save_compilation_info(llvm::StringRef file,
                                               llvm::StringRef directory,
                                               llvm::ArrayRef<const char*> arguments) {
    llvm::SmallVector<StringID, 32> canonical_args;
    llvm::SmallVector<StringID, 16> patch_args;

    /// Driver goes into canonical.
    canonical_args.push_back(strings.get(arguments[0]));

    bool remove_pch = false;

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

            /// Discard codegen-only options (shared with ToolchainProvider).
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
                    patch_args.push_back(strings.get(arg->getSpelling()));
                    llvm::StringRef value = arg->getValue(0);
                    if(!value.empty() && !path::is_absolute(value)) {
                        patch_args.push_back(strings.get(path::join(directory, value)));
                    } else {
                        patch_args.push_back(strings.get(value));
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
        canonical->arguments = persist_ids(canonical_args);
    }

    /// Build and dedup CompilationInfo.
    auto dir_id = strings.get(directory);
    auto info_id = infos.get(CompilationInfo{dir_id, canonical, patch_args});
    auto info = infos.get(info_id);
    if(info->patch.data() == patch_args.data()) {
        info->patch = persist_ids(patch_args);
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

        simdjson::ondemand::array args_arr;
        if(!obj["arguments"].get_array().get(args_arr)) {
            llvm::BumpPtrAllocator local;
            llvm::StringSaver saver(local);
            llvm::SmallVector<const char*, 32> args;
            for(auto arg_val: args_arr) {
                std::string_view sv;
                if(!arg_val.get_string().get(sv)) {
                    args.push_back(saver.save(llvm::StringRef(sv.data(), sv.size())).data());
                }
            }
            if(!args.empty()) {
                auto info = save_compilation_info(file_ref, dir_ref, args);
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
            auto path_id = paths.intern(file_ref);
            entries.push_back({path_id, info});
        }

        ++index;
    }

    // Sort by file path_id for binary search.
    ranges::sort(entries, {}, &CompilationEntry::file);

    return entries.size();
}

CompilationContext CompilationDatabase::lookup(llvm::StringRef file,
                                               const CommandOptions& options) {
    auto path_id = paths.intern(file);
    auto info = find_info(path_id);

    llvm::StringRef directory;
    std::vector<const char*> arguments;

    auto append_arg = [&](llvm::StringRef s) {
        arguments.emplace_back(strings.save(s).data());
    };

    auto append_ids = [&](llvm::ArrayRef<StringID> ids) {
        for(auto id: ids) {
            arguments.push_back(strings.get(id).data());
        }
    };

    if(info) {
        directory = strings.get(info->directory);

        if(options.query_toolchain) {
            // Build canonical-only args for the toolchain query.
            llvm::SmallVector<const char*, 32> canonical_chars;
            for(auto id: info->canonical->arguments) {
                canonical_chars.push_back(strings.get(id).data());
            }

            auto cached = toolchain_.query_cached(file, directory, canonical_chars);

            if(cached.empty()) {
                if(!options.suppress_logging) {
                    LOG_WARN("failed to query toolchain: {}", file);
                }
                // Fall back to simple canonical + patch.
                append_ids(info->canonical->arguments);
                append_ids(info->patch);
            } else {
                // Start with cc1 result.
                arguments.assign(cached.begin(), cached.end());
                arguments.pop_back();  // remove temp source file

                // Replace resource dir if needed.
                if(!resource_dir().empty()) {
                    llvm::StringRef old_resource_dir;
                    for(std::size_t i = 0; i + 1 < arguments.size(); ++i) {
                        if(arguments[i] == llvm::StringRef("-resource-dir")) {
                            old_resource_dir = arguments[i + 1];
                            break;
                        }
                    }
                    if(!old_resource_dir.empty() && old_resource_dir != resource_dir()) {
                        for(auto& arg: arguments) {
                            llvm::StringRef s(arg);
                            if(s.starts_with(old_resource_dir)) {
                                auto replaced =
                                    resource_dir().str() + s.substr(old_resource_dir.size()).str();
                                arg = strings.save(replaced).data();
                            }
                        }
                    }
                }

                // Replay patch args directly — already parsed, no re-parsing needed.
                append_ids(info->patch);

                // Fix -main-file-name to match the actual file.
                bool next_main_file = false;
                for(auto& arg: arguments) {
                    if(arg == llvm::StringRef("-main-file-name")) {
                        next_main_file = true;
                        continue;
                    }
                    if(next_main_file) {
                        arg = strings.save(path::filename(file)).data();
                        next_main_file = false;
                    }
                }
            }

            // Inject our resource dir if not already present.
            if(!resource_dir().empty()) {
                bool has_resource_dir = false;
                for(auto& arg: arguments) {
                    if(arg == llvm::StringRef("-resource-dir")) {
                        has_resource_dir = true;
                        break;
                    }
                }
                if(!has_resource_dir) {
                    append_arg("-resource-dir");
                    append_arg(resource_dir());
                }
            }
        } else {
            // Simple: canonical + patch. Already parsed and classified
            // at CDB load time — no re-parsing needed.
            append_ids(info->canonical->arguments);
            append_ids(info->patch);
        }

        // Apply remove filter to the final args (after query_toolchain so
        // the filter isn't lost when cc1 args replace the originals).
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

            auto saved_args = std::move(arguments);
            arguments.clear();
            arguments.push_back(saved_args.front());

            parser->parse(
                llvm::ArrayRef(saved_args).drop_front(),
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
                    render_arg_chars(arguments, *arg);
                },
                [](int, int) {});
        }

        for(auto& arg: options.append) {
            append_arg(arg);
        }
    } else if(file.ends_with(".cpp") || file.ends_with(".hpp") || file.ends_with(".cc")) {
        arguments = {"clang++", "-std=c++20"};
    } else {
        arguments = {"clang"};
    }

    arguments.emplace_back(paths.resolve(path_id).data());

    return CompilationContext(directory, std::move(arguments));
}

SearchConfig CompilationDatabase::lookup_search_config(llvm::StringRef file,
                                                       const CommandOptions& options) {
    auto path_id = paths.intern(file);
    auto info = find_info(path_id);

    // Only cache when remove/append are empty — custom options produce
    // per-call results that shouldn't pollute the shared cache.
    bool cacheable = info && options.remove.empty() && options.append.empty();

    if(cacheable) {
        auto key = ConfigCacheKey{info.ptr, options_bits(options)};
        auto cache_it = search_config_cache.find(key);
        if(cache_it != search_config_cache.end()) {
            return cache_it->second;
        }
    }

    auto ctx = lookup(file, options);
    auto config = extract_search_config(ctx.arguments, ctx.directory);

    if(cacheable) {
        auto key = ConfigCacheKey{info.ptr, options_bits(options)};
        search_config_cache.try_emplace(key, config);
    }
    return config;
}

bool CompilationDatabase::has_cached_configs() const {
    return !search_config_cache.empty();
}

ToolchainProvider& CompilationDatabase::toolchain() {
    return toolchain_;
}

llvm::StringRef CompilationDatabase::resolve_path(std::uint32_t path_id) {
    return paths.resolve(path_id);
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

std::string print_argv(llvm::ArrayRef<const char*> args) {
    std::string s = "[";
    if(!args.empty()) {
        s += args.consume_front();
        for(auto arg: args) {
            s += " ";
            s += arg;
        }
    }
    s += "]";
    return s;
}

}  // namespace clice
