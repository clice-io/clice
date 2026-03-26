#include "command/command.h"

#include <array>
#include <cctype>
#include <ranges>
#include <string_view>
#include <tuple>

#include "command/toolchain.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/StringSaver.h"

#include "clang/Driver/Driver.h"
#include "clang/Driver/Options.h"

namespace clice {

namespace {

namespace ranges = std::ranges;
namespace json = llvm::json;

using ID = clang::driver::options::ID;

}  // namespace

CompilationDatabase::CompilationDatabase() = default;

CompilationDatabase::~CompilationDatabase() = default;

object_ptr<CompilationInfo> CompilationDatabase::find_info(StringID path_id,
                                                            const void* context) const {
    auto it = files_.find(path_id);
    if(it == files_.end()) {
        return nullptr;
    }
    if(!context) {
        return it->second->info;
    }
    auto cur = it->second;
    while(cur) {
        if(cur->info.ptr == context) {
            return cur->info;
        }
        cur = cur->next;
    }
    return nullptr;
}

bool CompilationDatabase::is_discarded_option(unsigned id) {
    switch(id) {
        /// Input file and output — we manage these ourselves.
        case ID::OPT_INPUT:
        case ID::OPT_c:
        case ID::OPT_o:
        case ID::OPT_dxc_Fc:
        case ID::OPT_dxc_Fo:

        /// PCH building.
        case ID::OPT_emit_pch:
        case ID::OPT_include_pch:
        case ID::OPT__SLASH_Yu:
        case ID::OPT__SLASH_Fp:

        /// Dependency scan.
        case ID::OPT_E:
        case ID::OPT_M:
        case ID::OPT_MM:
        case ID::OPT_MD:
        case ID::OPT_MMD:
        case ID::OPT_MF:
        case ID::OPT_MT:
        case ID::OPT_MQ:
        case ID::OPT_MG:
        case ID::OPT_MP:
        case ID::OPT_show_inst:
        case ID::OPT_show_encoding:
        case ID::OPT_show_includes:
        case ID::OPT__SLASH_showFilenames:
        case ID::OPT__SLASH_showFilenames_:
        case ID::OPT__SLASH_showIncludes:
        case ID::OPT__SLASH_showIncludes_user:

        /// C++ modules — we handle these ourselves.
        case ID::OPT_fmodule_file:
        case ID::OPT_fmodule_output:
        case ID::OPT_fprebuilt_module_path:
            return true;

        default:
            return false;
    }
}

bool CompilationDatabase::is_user_content_option(unsigned id) {
    switch(id) {
        case ID::OPT_I:
        case ID::OPT_isystem:
        case ID::OPT_iquote:
        case ID::OPT_idirafter:
        case ID::OPT_D:
        case ID::OPT_U:
        case ID::OPT_include:
            return true;
        default:
            return false;
    }
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

void CompilationDatabase::render_arg(llvm::SmallVectorImpl<StringID>& out,
                                     llvm::opt::Arg& arg) {
    render_arg_to([&](llvm::StringRef s) { out.push_back(strings.get(s)); }, arg);
}

void CompilationDatabase::render_arg_chars(std::vector<const char*>& out,
                                           llvm::opt::Arg& arg) {
    render_arg_to([&](llvm::StringRef s) { out.push_back(strings.save(s).data()); }, arg);
}

llvm::ArrayRef<StringID> CompilationDatabase::persist_ids(llvm::ArrayRef<StringID> ids) {
    if(ids.empty())
        return {};
    auto* buf = allocator.Allocate<StringID>(ids.size());
    std::ranges::copy(ids, buf);
    return {buf, ids.size()};
}

object_ptr<CompilationInfo> CompilationDatabase::save_compilation_info(
    llvm::StringRef file,
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

    parser.parse(
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
            if(id == ID::OPT_Xclang && arg->getNumValues() == 1) {
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
                if((id == ID::OPT_I || id == ID::OPT_isystem ||
                    id == ID::OPT_iquote || id == ID::OPT_idirafter) &&
                   arg->getNumValues() == 1) {
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

object_ptr<CompilationInfo> CompilationDatabase::save_compilation_info(
    llvm::StringRef file,
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

void CompilationDatabase::insert_item(object_ptr<JSONItem> item) {
    auto [it, success] = files_.try_emplace(item->file_path, item);
    if(success) {
        return;
    }

    if(!it->second) {
        it->second = item;
        return;
    }

    auto cur = it->second;
    while(cur->next) {
        cur = cur->next;
    }
    cur->next = item;
}

void CompilationDatabase::delete_item(object_ptr<JSONItem> item) {
    auto it = files_.find(item->file_path);
    if(it == files_.end()) {
        return;
    }

    if(it->second == item) {
        it->second = item->next;
        return;
    }

    auto cur = it->second;
    while(cur->next) {
        if(cur->next == item) {
            cur->next = item->next;
            break;
        }
        cur = cur->next;
    }
}

std::vector<UpdateInfo> CompilationDatabase::update_source(JSONSource& source) {
    namespace ranges = std::ranges;
    std::vector<UpdateInfo> updates;

    // Invalidate SearchConfig cache — compilation entries are changing.
    search_config_cache.clear();

    ranges::sort(source.items, [](object_ptr<JSONItem> lhs, object_ptr<JSONItem> rhs) {
        return *lhs < *rhs;
    });

    auto it = ranges::find(sources, source.src_path, &JSONSource::src_path);
    if(it == sources.end()) {
        for(auto& item: source.items) {
            insert_item(item);
            updates.emplace_back(UpdateKind::Inserted, item->file_path, item->info.ptr);
        }

        sources.emplace_back(std::move(source));
    } else {
        auto& new_items = source.items;
        auto& old_items = it->items;

        auto it_new = new_items.begin();
        auto it_old = old_items.begin();

        while(it_new != new_items.end() && it_old != old_items.end()) {
            const auto& new_item = **it_new;
            const auto& old_item = **it_old;

            if(new_item == old_item) {
                updates.emplace_back(UpdateKind::Unchanged,
                                     new_item.file_path,
                                     new_item.info.ptr);
                ++it_new;
                ++it_old;
            } else if(new_item < old_item) {
                insert_item(*it_new);
                updates.emplace_back(UpdateKind::Inserted,
                                     new_item.file_path,
                                     new_item.info.ptr);
                ++it_new;
            } else {
                delete_item(*it_old);
                updates.emplace_back(UpdateKind::Deleted,
                                     old_item.file_path,
                                     old_item.info.ptr);
                ++it_old;
            }
        }

        while(it_new != new_items.end()) {
            insert_item(*it_new);
            updates.emplace_back(UpdateKind::Inserted,
                                 (*it_new)->file_path,
                                 (*it_new)->info.ptr);
            ++it_new;
        }

        while(it_old != old_items.end()) {
            delete_item(*it_old);
            updates.emplace_back(UpdateKind::Deleted,
                                 (*it_old)->file_path,
                                 (*it_old)->info.ptr);
            ++it_old;
        }

        it->items = std::move(source.items);
    }

    return updates;
}

std::vector<UpdateInfo> CompilationDatabase::load_compile_database(llvm::StringRef path) {
    auto content = llvm::MemoryBuffer::getFile(path);
    if(!content) {
        LOG_ERROR("Failed to read compilation database from {}. Reason: {}",
                  path,
                  content.getError());
        return {};
    }

    auto json_val = json::parse(content.get()->getBuffer());
    if(!json_val) {
        LOG_ERROR("Failed to parse compilation database from {}. Reason: {}",
                  path,
                  json_val.takeError());
        return {};
    }

    if(json_val->kind() != json::Value::Array) {
        LOG_ERROR(
            "Invalid compilation database format in {}. Reason: Root element must be an array.",
            path);
        return {};
    }

    JSONSource source;
    source.src_path = strings.get(path);

    for(size_t i = 0; i < json_val->getAsArray()->size(); ++i) {
        const auto& value = (*json_val->getAsArray())[i];
        if(value.kind() != json::Value::Object) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}. Reason: item is not an object.",
                path,
                i);
            continue;
        }

        auto& object = *value.getAsObject();
        auto directory = object.getString("directory");
        if(!directory) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}. Reason: 'directory' key is missing.",
                path,
                i);
            continue;
        }

        auto file = object.getString("file");
        if(!file) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}. Reason: 'file' key is missing.",
                path,
                i);
            continue;
        }

        auto arguments = object.getArray("arguments");
        auto command = object.getString("command");
        if(!arguments && !command) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}. Reason: neither 'arguments' nor 'command' key is present.",
                path,
                i);
            continue;
        }

        JSONItem item;
        item.json_src_path = source.src_path;
        item.file_path = strings.get(*file);
        if(arguments) {
            llvm::BumpPtrAllocator local;
            llvm::StringSaver saver(local);
            llvm::SmallVector<const char*, 32> agrs;
            for(auto& argument: *arguments) {
                if(argument.kind() == json::Value::String) {
                    agrs.emplace_back(saver.save(*argument.getAsString()).data());
                }
            }
            item.info = save_compilation_info(*file, *directory, agrs);
        } else if(command) {
            item.info = save_compilation_info(*file, *directory, *command);
        }
        source.items.emplace_back(items.save(item));
    }

    return update_source(source);
}

CompilationContext CompilationDatabase::lookup(llvm::StringRef file,
                                               const CommandOptions& options,
                                               const void* context) {
    auto path_id = strings.get(file);
    file = strings.get(path_id);
    auto info = find_info(path_id, context);

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
                arguments.pop_back(); // remove temp source file

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
            parser.parse(
                remove_strs,
                [&remove_args](Arg arg) { remove_args.emplace_back(std::move(arg)); },
                [](int, int) {});
            auto get_id = [](const Arg& arg) { return arg->getOption().getID(); };
            std::ranges::sort(remove_args, {}, get_id);

            auto saved_args = std::move(arguments);
            arguments.clear();
            arguments.push_back(saved_args.front());

            parser.parse(
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

    arguments.emplace_back(file.data());

    return CompilationContext(directory, std::move(arguments));
}

SearchConfig CompilationDatabase::lookup_search_config(llvm::StringRef file,
                                                       const CommandOptions& options,
                                                       const void* context) {
    auto path_id = strings.get(file);
    auto info = find_info(path_id, context);

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

    auto ctx = lookup(file, options, context);
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

std::optional<std::uint32_t> CompilationDatabase::get_option_id(llvm::StringRef argument) {
    auto& table = clang::driver::getDriverOptTable();

    llvm::SmallString<64> buffer = argument;

    if(argument.ends_with("=")) {
        buffer += "placeholder";
    }

    unsigned index = 1;
    std::array arguments = {"clang++", buffer.c_str(), "placeholder"};
    llvm::opt::InputArgList arg_list(arguments.data(), arguments.data() + arguments.size());

    if(auto arg = table.ParseOneArg(arg_list, index)) {
        return arg->getOption().getID();
    } else {
        return {};
    }
}

llvm::StringRef CompilationDatabase::resource_dir() {
    static std::string dir = [] {
        // Use address of this lambda to locate our binary via dladdr/proc.
        static int anchor;
        auto exe = llvm::sys::fs::getMainExecutable("", &anchor);
        if(exe.empty()) {
            return std::string{};
        }
        return clang::driver::Driver::GetResourcesPath(exe);
    }();
    return dir;
}

ToolchainProvider& CompilationDatabase::toolchain() {
    return toolchain_;
}

std::vector<ToolchainProvider::PendingEntry> CompilationDatabase::resolve_toolchain_entries(
    llvm::ArrayRef<std::pair<llvm::StringRef, const void*>> files) {
    std::vector<ToolchainProvider::PendingEntry> entries;
    entries.reserve(files.size());

    for(auto& [file, context]: files) {
        auto path_id = strings.get(file);
        auto stored_file = strings.get(path_id);
        auto info = find_info(path_id, context);

        if(!info || !info->canonical || info->canonical->arguments.empty()) {
            continue;
        }

        ToolchainProvider::PendingEntry entry;
        entry.file = stored_file;
        entry.directory = strings.get(info->directory);
        // Pass only canonical args — user-content options (-I/-D/-U/etc.)
        // are already separated into patch and will be replayed after
        // the toolchain query.
        entry.arguments.reserve(info->canonical->arguments.size());
        for(auto arg_id: info->canonical->arguments) {
            entry.arguments.push_back(strings.get(arg_id).data());
        }

        entries.push_back(std::move(entry));
    }

    return entries;
}

llvm::StringRef CompilationDatabase::resolve_path(std::uint32_t path_id) {
    return strings.get(path_id);
}

std::vector<const char*> CompilationDatabase::files() {
    std::vector<const char*> result;
    for(auto& [file, _]: files_) {
        result.emplace_back(strings.get(file).data());
    }
    return result;
}

llvm::StringRef CompilationDatabase::save_string(llvm::StringRef string) {
    return strings.save(string);
}

#ifdef CLICE_ENABLE_TEST

void CompilationDatabase::add_command(llvm::StringRef directory,
                                      llvm::StringRef file,
                                      llvm::ArrayRef<const char*> arguments) {
    JSONItem item;
    item.json_src_path = strings.get("fake");
    item.file_path = strings.get(file);
    item.info = save_compilation_info(file, directory, arguments);
    insert_item(items.save(item));
}

void CompilationDatabase::add_command(llvm::StringRef directory,
                                      llvm::StringRef file,
                                      llvm::StringRef command) {
    JSONItem item;
    item.json_src_path = strings.get("fake");
    item.file_path = strings.get(file);
    item.info = save_compilation_info(file, directory, command);
    insert_item(items.save(item));
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
