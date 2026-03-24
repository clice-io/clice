#include "compile/command.h"

#include <array>
#include <ranges>
#include <string_view>
#include <tuple>

#include "compile/driver.h"
#include "compile/toolchain.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/object_pool.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/StringSaver.h"

namespace clice {

namespace {

namespace ranges = std::ranges;
namespace json = llvm::json;

using StringID = StringSet::ID;

struct CompilationInfo {
    /// The working directory of the compilation.
    StringID directory = 0;

    /// The canonical compilation arguments(input file and output file are removed).
    llvm::ArrayRef<StringID> arguments;

    friend bool operator==(const CompilationInfo&, const CompilationInfo&) = default;
};

/// An item in the compilation database.
struct JSONItem {
    /// The path of the source json file, so that we can know where this
    /// json item from.
    StringID json_src_path = 0;

    /// The file path of this json item.
    StringID file_path = 0;

    /// The canonical compilation info of this item.
    object_ptr<CompilationInfo> info = {nullptr};

    /// A file may have multiple compilation commands, we use
    /// a chain to connect them. Note that this field does't
    /// get involved in equality judgement or hash computing.
    object_ptr<JSONItem> next = {nullptr};

    friend bool operator==(const JSONItem& lhs, const JSONItem& rhs) {
        return lhs.json_src_path == rhs.json_src_path && lhs.file_path == rhs.file_path &&
               lhs.info == rhs.info;
    }

    friend bool operator<(const JSONItem& lhs, const JSONItem& rhs) {
        return std::tie(lhs.file_path, lhs.info) < std::tie(rhs.file_path, rhs.info);
    }
};

struct JSONSource {
    /// The path of the source json file.
    StringID src_path;

    /// All json items in the json file, used for increment update.
    std::vector<object_ptr<JSONItem>> items;
};

using ID = clang::driver::options::ID;

}  // namespace

}  // namespace clice

namespace llvm {

template <>
struct DenseMapInfo<clice::CompilationInfo> {
    using T = clice::CompilationInfo;

    inline static T getEmptyKey() {
        return T(llvm::DenseMapInfo<std::uint32_t>::getEmptyKey());
    }

    inline static T getTombstoneKey() {
        return T(llvm::DenseMapInfo<std::uint32_t>::getTombstoneKey());
    }

    static unsigned getHashValue(const T& info) {
        return llvm::hash_combine(info.directory, llvm::hash_combine_range(info.arguments));
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        return lhs == rhs;
    }
};

template <>
struct DenseMapInfo<clice::JSONItem> {
    using T = clice::JSONItem;

    inline static T getEmptyKey() {
        return T(0, llvm::DenseMapInfo<std::uint32_t>::getEmptyKey());
    }

    inline static T getTombstoneKey() {
        return T(0, llvm::DenseMapInfo<std::uint32_t>::getTombstoneKey());
    }

    static unsigned getHashValue(const T& value) {
        return llvm::hash_combine(value.json_src_path, value.file_path, value.info.ptr);
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        return lhs == rhs;
    }
};

}  // namespace llvm

namespace clice {

struct CompilationDatabase::Impl {
    /// The memory pool which holds all elements of compilation database.
    /// We never try to release the memory until it destructs. So don't
    /// worry about the lifetime of allocated elements.
    llvm::BumpPtrAllocator allocator;

    /// Keep all strings.
    StringSet strings{allocator};

    /// Keep all items in the `compile_commands.json`.
    ObjectSet<JSONItem> items{allocator};

    /// Keep all canonical command infos, most of file actually
    /// have the same canonical command.
    ObjectSet<CompilationInfo> infos{allocator};

    /// All json source file.
    llvm::SmallVector<JSONSource> sources;

    /// All source files in the compilation database.
    llvm::DenseMap<StringID, object_ptr<JSONItem>> files;

    /// Cache of toolchain query results, keyed by canonical toolchain key.
    /// The key captures only flags that affect system path discovery (driver,
    /// target, sysroot, stdlib, etc.), so files sharing the same compiler
    /// configuration share one cached result.
    llvm::StringMap<std::vector<const char*>> toolchain_cache;

    /// Cache of SearchConfig per CompilationInfo pointer. Since infos are
    /// deduplicated by ObjectSet, the pointer uniquely identifies a compilation
    /// context. This avoids re-parsing arguments on repeated scans.
    llvm::DenseMap<const CompilationInfo*, SearchConfig> search_config_cache;

    /// The clang options we want to filter in all cases, like -c and -o.
    llvm::DenseSet<std::uint32_t> filtered_options;

    ArgumentParser parser{&allocator};

    /// Option IDs that affect system path discovery. These determine the
    /// toolchain cache key and are the only flags passed to the toolchain query.
    static bool is_toolchain_option(unsigned id) {
        switch(id) {
            case ID::OPT_target:
            case ID::OPT_target_legacy_spelling:
            case ID::OPT_isysroot:
            case ID::OPT__sysroot_EQ:
            case ID::OPT__sysroot:
            case ID::OPT_stdlib_EQ:
            case ID::OPT_gcc_toolchain:
            case ID::OPT_gcc_install_dir_EQ:
            case ID::OPT_nostdinc:
            case ID::OPT_nostdincxx:
            case ID::OPT_std_EQ: return true;
            default: return false;
        }
    }

    /// Extract toolchain-relevant flags from arguments using the clang argument
    /// parser. Returns both a cache key string and a minimal argument list for
    /// the toolchain query. Using the parser ensures all flag forms (joined,
    /// separate, etc.) are handled correctly.
    struct ToolchainExtract {
        std::string key;
        std::vector<const char*> query_args;
    };

    ToolchainExtract extract_toolchain_flags(this Impl& self,
                                             llvm::StringRef file,
                                             llvm::ArrayRef<const char*> arguments) {
        ToolchainExtract result;

        // Driver binary (first arg) — e.g. "clang++" vs "clang" affects language mode.
        result.key += arguments[0];
        result.key += '\0';

        // File extension affects language mode (C vs C++).
        result.key += path::extension(file);
        result.key += '\0';

        result.query_args.push_back(arguments[0]);

        self.parser.parse(
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

                // Render the argument back to query args, respecting the option's
                // render style (joined vs separate).
                switch(arg->getOption().getRenderStyle()) {
                    case llvm::opt::Option::RenderJoinedStyle: {
                        // e.g. -std=c++17, --target=x86_64-linux-gnu
                        llvm::SmallString<64> joined(arg->getSpelling());
                        if(arg->getNumValues() > 0) {
                            joined += arg->getValue(0);
                        }
                        result.query_args.push_back(self.strings.save(joined).data());
                        break;
                    }
                    case llvm::opt::Option::RenderSeparateStyle: {
                        // e.g. -target x86_64-linux-gnu, -isysroot /path
                        result.query_args.push_back(self.strings.save(arg->getSpelling()).data());
                        for(auto value: arg->getValues()) {
                            result.query_args.push_back(self.strings.save(value).data());
                        }
                        break;
                    }
                    default: {
                        // Flags (no value): -nostdinc, -nostdinc++
                        result.query_args.push_back(self.strings.save(arg->getSpelling()).data());
                        break;
                    }
                }
            },
            [](int, int) {
                // Ignore unknown arguments — they won't affect toolchain discovery.
            });

        return result;
    }

    /// Query toolchain with caching. Returns the cached cc1 args for the given
    /// toolchain key, running the expensive query only on cache miss.
    llvm::ArrayRef<const char*> query_toolchain_cached(this Impl& self,
                                                       llvm::StringRef file,
                                                       llvm::StringRef directory,
                                                       llvm::ArrayRef<const char*> arguments) {
        auto [key, query_args] = self.extract_toolchain_flags(file, arguments);
        auto it = self.toolchain_cache.find(key);
        if(it != self.toolchain_cache.end()) {
            return it->second;
        }

        LOG_WARN("Toolchain cache miss (spawning process): file={}, cache_size={}, key_len={}",
                 file,
                 self.toolchain_cache.size(),
                 key.size());

        auto callback = [&](const char* s) -> const char* {
            return self.strings.save(s).data();
        };
        toolchain::QueryParams params = {file, directory, query_args, callback};
        auto result = toolchain::query_toolchain(params);

        auto [entry, _] = self.toolchain_cache.try_emplace(std::move(key), std::move(result));
        return entry->second;
    }

    /// Check if an argument matches the source file path, handling
    /// Windows path separator differences (backslash vs forward slash).
    static bool is_same_file(llvm::StringRef argument, llvm::StringRef file) {
        if(argument == file) {
            return true;
        }

#ifdef _WIN32
        // On Windows, cmake may use backslashes in `arguments` but forward
        // slashes in `file`. Normalize and compare.
        if(argument.size() == file.size()) {
            for(std::size_t i = 0; i < argument.size(); i++) {
                char a = argument[i] == '\\' ? '/' : argument[i];
                char b = file[i] == '\\' ? '/' : file[i];
                if(a != b) {
                    return false;
                }
            }
            return true;
        }
#endif

        return false;
    }

    object_ptr<CompilationInfo> save_compilation_info(this Impl& self,
                                                      llvm::StringRef file,
                                                      llvm::StringRef directory,
                                                      llvm::ArrayRef<const char*> arguments) {
        llvm::SmallVector<StringID, 32> stored_arguments;

        self.parser.set_arguments(arguments);
        /// We don't want to parse all arguments here, it is time-consuming. But we
        /// want to remove output and input file from arguments. They are main reasons
        /// causing different file have different commands.
        for(unsigned it = 0; it != arguments.size(); it++) {
            llvm::StringRef argument = arguments[it];

            if(is_same_file(argument, file)) {
                continue;
            }

            /// All possible output options prefix.
            constexpr static std::string_view output_options[] = {
                "-o",
                "--output",
                "/o",
                "/Fo",
                "/Fe",
                "/Fd",
            };

            /// FIXME: This is a heuristic approach that covers the vast majority of cases, but
            /// theoretical corner cases exist. For example, `-oxx` might be an argument for another
            /// command, and processing it this way would lead to its incorrect removal. To fix
            /// these corner cases, it's necessary to parse the command line fully. Additionally,
            /// detailed benchmarks should be conducted to determine the time required for parsing
            /// command-line arguments in order to decide if it's worth doing so.
            if(ranges::any_of(output_options, [&](llvm::StringRef option) {
                   return argument.starts_with(option);
               })) {
                auto prev = it;
                auto arg = self.parser.parse_one(it);

                /// FIXME: How to handle parse error here?
                if(!arg) {
                    it = prev;
                    continue;
                }

                auto id = arg->getOption().getID();
                if(id == ID::OPT_o || id == ID::OPT_dxc_Fo || id == ID::OPT__SLASH_o ||
                   id == ID::OPT__SLASH_Fo || id == ID::OPT__SLASH_Fe) {
                    /// It will point to the next argument start but it also increases
                    /// in the next loop. So decrease it for not skipping next argument.
                    it -= 1;
                    continue;
                }

                /// This argument doesn't represent output file, just recovery it.
                it = prev;
            }

            /// FIXME: Handle response file.
            if(argument.starts_with("@")) {
                LOG_WARN(
                    "clice currently supports only one response file in the command, when loads {}",
                    file);
                continue;
            }

            stored_arguments.emplace_back(self.strings.get(argument));
        }

        auto info_id = self.infos.get({
            self.strings.get(directory),
            stored_arguments,
        });

        /// Note: check whether the arguments data are same as stored arguments,
        /// if so, we need allocate buffer for it to avoid dangling reference.
        auto info = self.infos.get(info_id);
        if(info->arguments.data() == stored_arguments.data()) {
            auto result = self.allocator.Allocate<StringID>(info->arguments.size());
            std::ranges::copy(info->arguments, result);
            info->arguments = {result, info->arguments.size()};
        }

        return info;
    }

    object_ptr<CompilationInfo> save_compilation_info(this Impl& self,
                                                      llvm::StringRef file,
                                                      llvm::StringRef directory,
                                                      llvm::StringRef command) {
        llvm::BumpPtrAllocator local;
        llvm::StringSaver saver(local);

        llvm::SmallVector<const char*, 32> arguments;

        /// FIXME: We need a better way to handle this.
        if(command.contains("cl.exe") || command.contains("clang-cl")) {
            llvm::cl::TokenizeWindowsCommandLineFull(command, saver, arguments);
        } else {
            llvm::cl::TokenizeGNUCommandLine(command, saver, arguments);
        }

        return self.save_compilation_info(file, directory, arguments);
    }

    void insert_item(this Impl& self, object_ptr<JSONItem> item) {
        auto [it, success] = self.files.try_emplace(item->file_path, item);
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

    void delete_item(this Impl& self, object_ptr<JSONItem> item) {
        auto it = self.files.find(item->file_path);
        if(it == self.files.end()) {
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

    auto update_source(this Impl& self, JSONSource& source) {
        std::vector<UpdateInfo> updates;

        /// We only need to sort the input source items, so that sources in self
        /// are already sorted.
        ranges::sort(source.items, [](object_ptr<JSONItem> lhs, object_ptr<JSONItem> rhs) {
            return *lhs < *rhs;
        });

        auto it = ranges::find(self.sources, source.src_path, &JSONSource::src_path);
        if(it == self.sources.end()) {
            for(auto& item: source.items) {
                self.insert_item(item);
                updates.emplace_back(UpdateKind::Inserted, item->file_path, item->info.ptr);
            }

            self.sources.emplace_back(std::move(source));
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
                    self.insert_item(*it_new);
                    updates.emplace_back(UpdateKind::Inserted,
                                         new_item.file_path,
                                         new_item.info.ptr);
                    ++it_new;
                } else {
                    self.delete_item(*it_old);
                    updates.emplace_back(UpdateKind::Deleted,
                                         old_item.file_path,
                                         old_item.info.ptr);
                    ++it_old;
                }
            }

            while(it_new != new_items.end()) {
                self.insert_item(*it_new);
                updates.emplace_back(UpdateKind::Inserted,
                                     (*it_new)->file_path,
                                     (*it_new)->info.ptr);
                ++it_new;
            }

            while(it_old != old_items.end()) {
                self.delete_item(*it_old);
                updates.emplace_back(UpdateKind::Deleted,
                                     (*it_old)->file_path,
                                     (*it_old)->info.ptr);
                ++it_old;
            }

            it->items = std::move(source.items);
        }

        return updates;
    }

    auto mangle_command(this Impl& self,
                        llvm::StringRef file,
                        const CompilationInfo& info,
                        const CommandOptions& options) {
        llvm::StringRef directory = self.strings.get(info.directory);
        llvm::SmallVector<const char*, 32> arguments;
        for(auto arg: info.arguments) {
            arguments.emplace_back(self.strings.get(arg).data());
        }

        /// Store the final result arguments.
        llvm::SmallVector<const char*, 16> final_arguments;

        auto add_string = [&](llvm::StringRef argument) {
            auto saved = self.strings.save(argument);
            final_arguments.emplace_back(saved.data());
        };

        /// Rewrite the argument to filter arguments, we basically reimplement
        /// the logic of `Arg::render` to use our allocator to allocate memory.
        auto add_argument = [&](llvm::opt::Arg& arg) {
            switch(arg.getOption().getRenderStyle()) {
                case llvm::opt::Option::RenderValuesStyle: {
                    for(auto value: arg.getValues()) {
                        add_string(value);
                    }
                    break;
                }

                case llvm::opt::Option::RenderSeparateStyle: {
                    add_string(arg.getSpelling());
                    for(auto value: arg.getValues()) {
                        add_string(value);
                    }
                    break;
                }

                case llvm::opt::Option::RenderJoinedStyle: {
                    llvm::SmallString<256> first = {arg.getSpelling(), arg.getValue(0)};
                    add_string(first);
                    for(auto value: llvm::ArrayRef(arg.getValues()).drop_front()) {
                        add_string(value);
                    }
                    break;
                }

                case llvm::opt::Option::RenderCommaJoinedStyle: {
                    llvm::SmallString<256> buffer = arg.getSpelling();
                    for(auto i = 0; i < arg.getNumValues(); i++) {
                        if(i) {
                            buffer += ',';
                        }
                        buffer += arg.getValue(i);
                    }
                    add_string(buffer);
                    break;
                }
            }
        };

        /// Append driver sperately
        add_string(arguments.front());

        using Arg = std::unique_ptr<llvm::opt::Arg>;
        auto on_error = [&](int index, int count) {
            LOG_WARN("missing argument index: {}, count: {} when parse: {}", index, count, file);
        };

        /// Prepare for removing arguments.
        llvm::SmallVector<const char*> remove;
        for(auto& arg: options.remove) {
            remove.push_back(self.strings.save(arg).data());
        }

        /// FIXME: Handle unknow remove arguments.
        llvm::SmallVector<Arg> known_remove_args;
        self.parser.parse(
            remove,
            [&known_remove_args](Arg arg) { known_remove_args.emplace_back(std::move(arg)); },
            on_error);
        auto get_id = [](const Arg& arg) {
            return arg->getOption().getID();
        };
        ranges::sort(known_remove_args, {}, get_id);

        bool remove_pch = false;

        /// FIXME: Append the commands from response file.
        self.parser.parse(
            llvm::ArrayRef(arguments).drop_front(),
            [&](Arg arg) {
                auto& opt = arg->getOption();
                auto id = opt.getID();

                /// Filter options we don't need.
                if(self.filtered_options.contains(id)) {
                    return;
                }

                /// Remove arguments in the remove list.
                auto range = ranges::equal_range(known_remove_args, id, {}, get_id);
                for(auto& remove: range) {
                    /// Match the -I*.
                    if(remove->getNumValues() == 1 && remove->getValue(0) == llvm::StringRef("*")) {
                        return;
                    }

                    /// Compare each value, convert `const char*` to `llvm::StringRef` for
                    /// comparing.
                    if(ranges::equal(
                           arg->getValues(),
                           remove->getValues(),
                           [](llvm::StringRef lhs, llvm::StringRef rhs) { return lhs == rhs; })) {
                        return;
                    }
                }

                /// For arguments -I<dir>, convert directory to absolute path.
                /// i.e xmake will generate commands in this style.
                if(id == ID::OPT_I && arg->getNumValues() == 1) {
                    add_string("-I");
                    llvm::StringRef value = arg->getValue(0);
                    if(!value.empty() && !path::is_absolute(value)) {
                        add_string(path::join(directory, value));
                    } else {
                        add_string(value);
                    }
                    return;
                }

                /// A workaround to remove extra PCH when cmake generate PCH flags for clang.
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

                add_argument(*arg);
            },
            on_error);

        /// FIXME: Do we want to parse append arguments also?
        for(auto& arg: options.append) {
            add_string(arg);
        }

        return llvm::ArrayRef(final_arguments).vec();
    }
};

CompilationDatabase::CompilationDatabase() : self(std::make_unique<CompilationDatabase::Impl>()) {
    constexpr static std::array filtered_options = {
        /// Remove the input file, we will add input file ourselves.
        ID::OPT_INPUT,

        /// -c and -o are meaningless for frontend.
        ID::OPT_c,
        ID::OPT_o,
        ID::OPT_dxc_Fc,
        ID::OPT_dxc_Fo,

        /// Remove all ID related to PCH building.
        ID::OPT_emit_pch,
        ID::OPT_include_pch,
        ID::OPT__SLASH_Yu,
        ID::OPT__SLASH_Fp,

        /// Remove all ID related to dependency scan.
        ID::OPT_E,
        ID::OPT_M,
        ID::OPT_MM,
        ID::OPT_MD,
        ID::OPT_MMD,
        ID::OPT_MF,
        ID::OPT_MT,
        ID::OPT_MQ,
        ID::OPT_MG,
        ID::OPT_MP,
        ID::OPT_show_inst,
        ID::OPT_show_encoding,
        ID::OPT_show_includes,
        ID::OPT__SLASH_showFilenames,
        ID::OPT__SLASH_showFilenames_,
        ID::OPT__SLASH_showIncludes,
        ID::OPT__SLASH_showIncludes_user,

        /// Remove all ID related to C++ module, we will
        /// build module and set deps ourselves.
        ID::OPT_fmodule_file,
        ID::OPT_fmodule_output,
        ID::OPT_fprebuilt_module_path,
    };

    for(auto opt: filtered_options) {
        self->filtered_options.insert(opt);
    }
}

CompilationDatabase::CompilationDatabase(CompilationDatabase&& other) = default;

CompilationDatabase& CompilationDatabase::operator=(CompilationDatabase&& other) = default;

CompilationDatabase::~CompilationDatabase() = default;

std::vector<UpdateInfo> CompilationDatabase::load_compile_database(llvm::StringRef path) {
    auto content = llvm::MemoryBuffer::getFile(path);
    if(!content) {
        LOG_ERROR("Failed to read compilation database from {}. Reason: {}",
                  path,
                  content.getError());
        return {};
    }

    auto json = json::parse(content.get()->getBuffer());
    if(!json) {
        LOG_ERROR("Failed to parse compilation database from {}. Reason: {}",
                  path,
                  json.takeError());
        return {};
    }

    if(json->kind() != json::Value::Array) {
        LOG_ERROR(
            "Invalid compilation database format in {}. Reason: Root element must be an array.",
            path);
        return {};
    }

    JSONSource source;
    source.src_path = self->strings.get(path);

    for(size_t i = 0; i < json->getAsArray()->size(); ++i) {
        const auto& value = (*json->getAsArray())[i];
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
        item.file_path = self->strings.get(*file);
        if(arguments) {
            llvm::BumpPtrAllocator local;
            llvm::StringSaver saver(local);
            llvm::SmallVector<const char*, 32> agrs;
            for(auto& argument: *arguments) {
                if(argument.kind() == json::Value::String) {
                    agrs.emplace_back(saver.save(*argument.getAsString()).data());
                }
            }
            item.info = self->save_compilation_info(*file, *directory, agrs);
        } else if(command) {
            item.info = self->save_compilation_info(*file, *directory, *command);
        }
        source.items.emplace_back(self->items.save(item));
    }

    return self->update_source(source);
}

CompilationContext CompilationDatabase::lookup(llvm::StringRef file,
                                               const CommandOptions& options,
                                               const void* context) {
    object_ptr<CompilationInfo> info = nullptr;

    auto path_id = self->strings.get(file);
    file = self->strings.get(path_id);

    auto it = self->files.find(path_id);
    if(it != self->files.end()) [[unlikely]] {
        if(!context) {
            /// If context is not provided, we just use the first.
            info = it->second->info;
        } else {
            /// Otherwise find the corresponding one.
            auto cur = it->second;
            while(cur) {
                if(cur->info.ptr == context) {
                    info = cur->info;
                    break;
                }
                cur = cur->next;
            }
        }
    }

    llvm::StringRef directory;
    std::vector<const char*> arguments;

    if(info) {
        directory = self->strings.get(info->directory);
        arguments = self->mangle_command(file, *info, options);
        // TODO: other c++ suffixes
    } else if(file.ends_with(".cpp") || file.ends_with(".hpp") || file.ends_with(".cc")) {
        arguments = {"clang++", "-std=c++20"};
    } else {
        arguments = {"clang"};
    }

    auto append_arg = [&](llvm::StringRef s) {
        arguments.emplace_back(self->strings.save(s).data());
    };

    if(options.resource_dir) {
        append_arg("-resource-dir");
        append_arg(fs::resource_dir);
    }

    if(info && options.query_toolchain) {
        // Save user-level include paths before replacing with cc1 args.
        // The cached toolchain query uses minimal args (no -I/-D/-W etc.)
        // for cache efficiency, so user include paths must be injected back.
        auto user_args = std::move(arguments);

        auto cached = self->query_toolchain_cached(file, directory, user_args);

        if(cached.empty()) {
            LOG_WARN("failed to query toolchain: {}", file);
            arguments = std::move(user_args);
        } else {
            // Start with cc1 result (has system paths, driver flags, etc.).
            arguments.assign(cached.begin(), cached.end());

            // Remove the temp source file that was appended during query.
            arguments.pop_back();

            // Inject user include paths (-I, -isystem, -iquote) from the
            // original mangled args into the cc1 result.
            self->parser.parse(
                llvm::ArrayRef(user_args).drop_front(),
                [&](std::unique_ptr<llvm::opt::Arg> arg) {
                    auto id = arg->getOption().getID();
                    if(id == ID::OPT_I || id == ID::OPT_isystem || id == ID::OPT_iquote) {
                        append_arg(arg->getSpelling());
                        for(auto value: arg->getValues()) {
                            append_arg(value);
                        }
                    }
                },
                [](int, int) {});

            // Fix -main-file-name to match the actual file.
            bool next_main_file = false;
            for(auto& arg: arguments) {
                if(arg == llvm::StringRef("-main-file-name")) {
                    next_main_file = true;
                    continue;
                }

                if(next_main_file) {
                    arg = self->strings.save(path::filename(file)).data();
                    next_main_file = false;
                }
            }
        }
    }

    arguments.emplace_back(file.data());

    return CompilationContext(directory, std::move(arguments));
}

SearchConfig CompilationDatabase::extract_search_config(const CompilationContext& ctx) {
    SearchConfig config;

    auto add_dir = [&](llvm::StringRef path, bool is_system) {
        llvm::SmallString<256> abs_path(path);
        if(!llvm::sys::path::is_absolute(abs_path)) {
            llvm::sys::fs::make_absolute(ctx.directory, abs_path);
        }
        llvm::sys::path::remove_dots(abs_path, true);

        if(is_system && config.angled_start_idx == config.dirs.size()) {
            config.angled_start_idx = static_cast<unsigned>(config.dirs.size());
        }

        config.dirs.push_back({abs_path.str().str()});
    };

    self->parser.parse(
        llvm::ArrayRef(ctx.arguments).drop_front(),
        [&](std::unique_ptr<llvm::opt::Arg> arg) {
            auto id = arg->getOption().getID();
            switch(id) {
                case ID::OPT_I: add_dir(arg->getValue(), false); break;
                case ID::OPT_isystem:
                case ID::OPT_internal_isystem:
                case ID::OPT_internal_externc_isystem: add_dir(arg->getValue(), true); break;
                case ID::OPT_iquote: add_dir(arg->getValue(), false); break;
                default: break;
            }
        },
        [](int, int) {});

    return config;
}

SearchConfig CompilationDatabase::lookup_search_config(llvm::StringRef file,
                                                       const CommandOptions& options,
                                                       const void* context) {
    // Resolve to the internal CompilationInfo pointer for cache lookup.
    auto path_id = self->strings.get(file);
    auto it = self->files.find(path_id);
    const CompilationInfo* info_ptr = nullptr;
    if(it != self->files.end()) {
        if(!context) {
            info_ptr = it->second->info.ptr;
        } else {
            auto cur = it->second;
            while(cur) {
                if(cur->info.ptr == context) {
                    info_ptr = cur->info.ptr;
                    break;
                }
                cur = cur->next;
            }
        }
    }

    if(info_ptr) {
        auto cache_it = self->search_config_cache.find(info_ptr);
        if(cache_it != self->search_config_cache.end()) {
            return cache_it->second;
        }
    }

    auto ctx = lookup(file, options, context);
    auto config = extract_search_config(ctx);

    if(info_ptr) {
        self->search_config_cache.try_emplace(info_ptr, config);
    }
    return config;
}

bool CompilationDatabase::has_cached_configs() const {
    return !self->search_config_cache.empty();
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

std::vector<CompilationDatabase::ToolchainQuery> CompilationDatabase::get_pending_toolchain_queries(
    llvm::ArrayRef<std::pair<llvm::StringRef, const void*>> files) {
    // Extract the full toolchain key for every context and deduplicate.
    // The key includes driver + extension + toolchain-affecting flags
    // (e.g. -std=, -target, -isysroot), so contexts with different flags
    // produce different keys and need separate queries.
    llvm::StringMap<bool> seen_keys;
    std::vector<ToolchainQuery> queries;

    for(auto& [file, context]: files) {
        auto path_id = self->strings.get(file);
        auto stored_file = self->strings.get(path_id);

        object_ptr<CompilationInfo> info = nullptr;
        auto it = self->files.find(path_id);
        if(it != self->files.end()) {
            if(!context) {
                info = it->second->info;
            } else {
                auto cur = it->second;
                while(cur) {
                    if(cur->info.ptr == context) {
                        info = cur->info;
                        break;
                    }
                    cur = cur->next;
                }
            }
        }

        if(!info || info->arguments.empty()) {
            continue;
        }

        llvm::SmallVector<const char*, 32> raw_args;
        for(auto arg_id: info->arguments) {
            raw_args.push_back(self->strings.get(arg_id).data());
        }

        auto [key, query_args] = self->extract_toolchain_flags(stored_file, raw_args);

        // Skip if already cached or already queued.
        if(self->toolchain_cache.count(key) || !seen_keys.try_emplace(key, true).second) {
            continue;
        }

        LOG_DEBUG("Pre-warm: new toolchain key (len={}) for file={}", key.size(), stored_file);
        auto directory = self->strings.get(info->directory);
        queries.push_back({std::move(key), std::move(query_args), stored_file, directory});
    }

    LOG_INFO("Pre-warm: {} unique keys from {} contexts, {} queries needed",
             seen_keys.size(),
             files.size(),
             queries.size());
    return queries;
}

void CompilationDatabase::inject_toolchain_results(
    llvm::ArrayRef<CompilationDatabase::ToolchainResult> results) {
    for(auto& result: results) {
        if(self->toolchain_cache.count(result.key)) {
            continue;
        }
        std::vector<const char*> saved;
        saved.reserve(result.cc1_args.size());
        for(auto& arg: result.cc1_args) {
            saved.push_back(self->strings.save(arg).data());
        }
        self->toolchain_cache.try_emplace(result.key, std::move(saved));
    }
}

llvm::StringRef CompilationDatabase::resolve_path(std::uint32_t path_id) {
    return self->strings.get(path_id);
}

std::vector<const char*> CompilationDatabase::files() {
    std::vector<const char*> result;
    for(auto& [file, _]: self->files) {
        result.emplace_back(self->strings.get(file).data());
    }
    return result;
}

llvm::StringRef CompilationDatabase::save_string(llvm::StringRef string) {
    return self->strings.save(string);
}

#ifdef CLICE_ENABLE_TEST

void CompilationDatabase::add_command(llvm::StringRef directory,
                                      llvm::StringRef file,

                                      llvm::ArrayRef<const char*> arguments) {
    JSONItem item;
    item.json_src_path = self->strings.get("fake");
    item.file_path = self->strings.get(file);
    item.info = self->save_compilation_info(file, directory, arguments);
    self->insert_item(self->items.save(item));
}

void CompilationDatabase::add_command(llvm::StringRef directory,
                                      llvm::StringRef file,
                                      llvm::StringRef command) {
    JSONItem item;
    item.json_src_path = self->strings.get("fake");
    item.file_path = self->strings.get(file);
    item.info = self->save_compilation_info(file, directory, command);
    self->insert_item(self->items.save(item));
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
