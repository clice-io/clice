#include "Compiler/Command.h"
#include "Compiler/Compilation.h"
#include "Support/FileSystem.h"
#include "Support/Logging.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Program.h"
#include "Driver.h"

namespace llvm {

template <>
struct DenseMapInfo<llvm::ArrayRef<const char*>> {
    using T = llvm::ArrayRef<const char*>;

    inline static T getEmptyKey() {
        return T(reinterpret_cast<T::const_pointer>(~0), T::size_type(0));
    }

    inline static T getTombstoneKey() {
        return T(reinterpret_cast<T::const_pointer>(~1), T::size_type(0));
    }

    static unsigned getHashValue(const T& value) {
        return llvm::hash_combine_range(value.begin(), value.end());
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        return lhs == rhs;
    }
};

}  // namespace llvm

namespace clice {

namespace options = clang::driver::options;

struct CommandInfo {
    /// TODO: add sysroot or no stdinc command info.
    llvm::StringRef directory;

    /// The canonical command list.
    llvm::ArrayRef<const char*> arguments;

    /// The extra command @...
    llvm::StringRef response_file;

    /// The original index of the response file argument in the command list.
    std::uint32_t response_file_index = 0;
};

struct CompilationDatabase::Impl {
    /// The memory pool to hold all cstring and command list.
    llvm::BumpPtrAllocator allocator;

    /// A cache between input string and its cache cstring
    /// in the allocator, make sure end with `\0`.
    llvm::DenseSet<llvm::StringRef> string_cache;

    /// A cache between input command and its cache array
    /// in the allocator.
    llvm::DenseSet<llvm::ArrayRef<const char*>> arguments_cache;

    /// The clang options we want to filter in all cases, like -c and -o.
    llvm::DenseSet<std::uint32_t> filtered_options;

    /// A map between file path and its canonical command list.
    llvm::DenseMap<const char*, CommandInfo> command_infos;

    /// A map between driver path and its query driver info.
    llvm::DenseMap<const char*, DriverInfo> driver_infos;

    ArgumentParser parser = {&allocator};

    using Self = CompilationDatabase::Impl;

    auto save_string(this Self& self, llvm::StringRef string) -> llvm::StringRef {
        assert(!string.empty() && "expected non empty string");
        auto it = self.string_cache.find(string);

        /// If we already store the argument, reuse it.
        if(it != self.string_cache.end()) {
            return *it;
        }

        /// Allocate for new string.
        const auto size = string.size();
        auto ptr = self.allocator.Allocate<char>(size + 1);
        std::memcpy(ptr, string.data(), size);
        ptr[size] = '\0';

        /// Insert it to cache.
        auto result = llvm::StringRef(ptr, size);
        self.string_cache.insert(result);
        return result;
    }

    auto save_cstring_list(this Self& self, llvm::ArrayRef<const char*> arguments)
        -> llvm::ArrayRef<const char*> {
        auto it = self.arguments_cache.find(arguments);

        /// If we already store the argument, reuse it.
        if(it != self.arguments_cache.end()) {
            return *it;
        }

        /// Allocate for new array.
        const auto size = arguments.size();
        auto ptr = self.allocator.Allocate<const char*>(size);
        ranges::copy(arguments, ptr);

        /// Insert it to cache.
        auto result = llvm::ArrayRef<const char*>(ptr, size);
        self.arguments_cache.insert(result);
        return result;
    }

    auto process_command(this Self& self,
                         llvm::StringRef file,
                         const CommandInfo& info,
                         const CommandOptions& options) -> std::vector<const char*> {

        /// Store the final result arguments.
        llvm::SmallVector<const char*, 16> final_arguments;

        auto add_string = [&](llvm::StringRef argument) {
            auto saved = self.save_string(argument);
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
        add_string(info.arguments.front());

        using Arg = std::unique_ptr<llvm::opt::Arg>;
        auto on_error = [&](int index, int count) {
            LOGGING_WARN("missing argument index: {}, count: {} when parse: {}",
                         index,
                         count,
                         file);
        };

        /// Prepare for removing arguments.
        llvm::SmallVector<const char*> remove;
        for(auto& arg: options.remove) {
            remove.push_back(self.save_string(arg).data());
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
            info.arguments.drop_front(),
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
                if(id == options::OPT_I && arg->getNumValues() == 1) {
                    add_string("-I");
                    llvm::StringRef value = arg->getValue(0);
                    if(!value.empty() && !path::is_absolute(value)) {
                        add_string(path::join(info.directory, value));
                    } else {
                        add_string(value);
                    }
                    return;
                }

                /// A workaround to remove extra PCH when cmake generate PCH flags for clang.
                if(id == options::OPT_Xclang && arg->getNumValues() == 1) {
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

    auto guess_or_fallback(this Self& self, llvm::StringRef file) -> LookupInfo {
        // Try to guess command from other file in same directory or parent directory
        llvm::StringRef dir = path::parent_path(file);

        // Search up to 3 levels of parent directories
        int up_level = 0;
        while(!dir.empty() && up_level < 3) {
            // If any file in the directory has a command, use that command
            for(const auto& [other_file, info]: self.command_infos) {
                llvm::StringRef other = other_file;
                // Filter case that dir is /path/to/foo and there's another directory
                // /path/to/foobar
                if(other.starts_with(dir) &&
                   (other.size() == dir.size() || path::is_separator(other[dir.size()]))) {
                    LOGGING_INFO("Guess command for:{}, from existed file: {}", file, other_file);
                    return LookupInfo{info.directory, info.arguments};
                }
            }
            dir = path::parent_path(dir);
            up_level += 1;
        }

        /// FIXME: use a better default case.
        // Fallback to default case.
        LookupInfo info;
        constexpr const char* fallback[] = {"clang++", "-std=c++20"};
        for(const char* arg: fallback) {
            info.arguments.emplace_back(self.save_string(arg).data());
        }
        return info;
    }
};

using ID = options::ID;
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

CompilationDatabase::CompilationDatabase() : self(std::make_unique<CompilationDatabase::Impl>()) {
    for(auto opt: filtered_options) {
        self->filtered_options.insert(opt);
    }
}

CompilationDatabase::CompilationDatabase(CompilationDatabase&& other) = default;

CompilationDatabase& CompilationDatabase::operator= (CompilationDatabase&& other) = default;

CompilationDatabase::~CompilationDatabase() = default;

std::optional<std::uint32_t> CompilationDatabase::get_option_id(llvm::StringRef argument) {
    auto& table = clang::driver::getDriverOptTable();

    llvm::SmallString<64> buffer = argument;

    if(argument.ends_with("=")) {
        buffer += "placeholder";
    }

    unsigned index = 0;
    std::array arguments = {buffer.c_str(), "placeholder"};
    llvm::opt::InputArgList arg_list(arguments.data(), arguments.data() + arguments.size());

    if(auto arg = table.ParseOneArg(arg_list, index)) {
        return arg->getOption().getID();
    } else {
        return {};
    }
}

auto CompilationDatabase::save_string(llvm::StringRef string) -> llvm::StringRef {
    return self->save_string(string);
}

auto CompilationDatabase::query_driver(llvm::StringRef driver)
    -> std::expected<DriverInfo, toolchain::QueryDriverError> {
    driver = self->save_string(driver).data();
    auto it = self->driver_infos.find(driver.data());
    if(it != self->driver_infos.end()) {
        return it->second;
    }

    auto driver_info = toolchain::query_driver(driver);
    if(!driver_info) {
        return std::unexpected(driver_info.error());
    }

    DriverInfo info;
    info.target = self->save_string(driver_info->target);

    llvm::SmallVector<const char*> includes;
    for(llvm::StringRef include: driver_info->includes) {
        llvm::SmallString<64> buffer;

        /// Make sure the path is absolute, otherwise it may be
        /// "/usr/lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13", which
        /// interferes with our determination of the resource directory
        auto err = fs::real_path(include, buffer);
        include = buffer;

        /// Remove resource dir of the driver.
        if(err ||
           include.contains("lib/gcc")
           /// FIXME: Only for windows, for Mac removing default resource dir
           /// may result in unexpected error. Figure out it.
           || include.contains("lib\\clang")) {
            continue;
        }
        includes.emplace_back(self->save_string(include).data());
    }

    info.system_includes = self->save_cstring_list(includes);
    self->driver_infos.try_emplace(driver.data(), info);
    return info;
}

auto CompilationDatabase::update_command(llvm::StringRef directory,
                                         llvm::StringRef file,
                                         llvm::ArrayRef<const char*> arguments) -> UpdateInfo {
    self->parser.set_arguments(arguments);

    file = self->save_string(file);
    directory = self->save_string(directory);

    llvm::StringRef response_file;
    std::uint32_t response_file_index = 0;

    llvm::SmallVector<const char*> canonical_arguments;

    /// We don't want to parse all arguments here, it is time-consuming. But we
    /// want to remove output and input file from arguments. They are main reasons
    /// causing different file have different commands.
    for(unsigned it = 0; it != arguments.size(); it++) {
        llvm::StringRef argument = arguments[it];

        /// FIXME: Is it possible that file in command and field are different?
        if(argument == file) {
            continue;
        }

        /// All possible output options prefix.
        constexpr static std::string_view output_options[] = {
            "-o",
            "--output",
#ifdef _WIN32
            "/o",
            "/Fo",
            "/Fe",
#endif
        };

        /// FIXME: This is a heuristic approach that covers the vast majority of cases, but
        /// theoretical corner cases exist. For example, `-oxx` might be an argument for another
        /// command, and processing it this way would lead to its incorrect removal. To fix
        /// these corner cases, it's necessary to parse the command line fully. Additionally,
        /// detailed benchmarks should be conducted to determine the time required for parsing
        /// command-line arguments in order to decide if it's worth doing so.
        if(ranges::any_of(output_options,
                          [&](llvm::StringRef option) { return argument.starts_with(option); })) {
            auto prev = it;
            auto arg = self->parser.parse_one(it);

            /// FIXME: How to handle parse error here?
            if(!arg) {
                it = prev;
                continue;
            }

            auto id = arg->getOption().getID();
            if(id == options::OPT_o || id == options::OPT_dxc_Fo || id == options::OPT__SLASH_o ||
               id == options::OPT__SLASH_Fo || id == options::OPT__SLASH_Fe) {
                /// It will point to the next argument start but it also increases
                /// in the next loop. So decrease it for not skipping next argument.
                it -= 1;
                continue;
            }

            /// This argument doesn't represent output file, just recovery it.
            it = prev;
        }

        /// Handle response file.
        if(argument.starts_with("@")) {
            if(!response_file.empty()) {
                LOGGING_WARN(
                    "clice currently supports only one response file in the command, when loads {}",
                    file);
            }
            response_file = self->save_string(argument);
            response_file_index = it;
            continue;
        }

        canonical_arguments.push_back(self->save_string(argument).data());
    }

    /// Cache the canonical arguments
    arguments = self->save_cstring_list(canonical_arguments);

    UpdateKind kind = UpdateKind::Unchange;
    CommandInfo info = {
        directory,
        arguments,
        response_file,
        response_file_index,
    };

    auto [it, success] = self->command_infos.try_emplace(file.data(), info);
    if(success) {
        /// If successfully inserted, we are loading new file.
        kind = UpdateKind::Create;
    } else {
        /// If failed to insert, compare whether need to update. Because we cache
        /// all the ref structure here, so just comparing the pointer is fine.
        auto& old_info = it->second;
        if(old_info.directory.data() != info.directory.data() ||
           old_info.arguments.data() != info.arguments.data() ||
           old_info.response_file.data() != info.response_file.data() ||
           old_info.response_file_index != info.response_file_index) {
            kind = UpdateKind::Update;
            old_info = info;
        }
    }

    return UpdateInfo{kind, file};
}

auto CompilationDatabase::update_command(llvm::StringRef directory,
                                         llvm::StringRef file,
                                         llvm::StringRef command) -> UpdateInfo {
    llvm::BumpPtrAllocator local;
    llvm::StringSaver saver(local);

    llvm::SmallVector<const char*, 32> arguments;
    auto [driver, _] = command.split(' ');
    driver = path::filename(driver);
    driver.consume_back(".exe");

    /// FIXME: Use a better to handle this.
    if(driver.ends_with("cl") || driver.starts_with("clang-cl")) {
        llvm::cl::TokenizeWindowsCommandLineFull(command, saver, arguments);
    } else {
        llvm::cl::TokenizeGNUCommandLine(command, saver, arguments);
    }

    return this->update_command(directory, file, arguments);
}

auto CompilationDatabase::load_commands(llvm::StringRef json_content, llvm::StringRef workspace)
    -> std::expected<std::vector<UpdateInfo>, std::string> {
    std::vector<UpdateInfo> infos;

    auto json = json::parse(json_content);
    if(!json) {
        return std::unexpected(std::format("parse json failed: {}", json.takeError()));
    }

    if(json->kind() != json::Value::Array) {
        return std::unexpected("compile_commands.json must be an array of object");
    }

    /// FIXME: warn illegal item.
    for(auto& item: *json->getAsArray()) {
        /// Ignore non-object item.
        if(item.kind() != json::Value::Object) {
            continue;
        }

        auto& object = *item.getAsObject();

        auto directory = object.getString("directory");
        if(!directory) {
            continue;
        }

        /// Always store absolute path of source file.
        std::string source;
        if(auto file = object.getString("file")) {
            source = path::is_absolute(*file) ? file->str() : path::join(*directory, *file);
        } else {
            continue;
        }

        if(auto arguments = object.getArray("arguments")) {
            /// Construct cstring array.
            llvm::BumpPtrAllocator local;
            llvm::StringSaver saver(local);
            llvm::SmallVector<const char*, 32> carguments;

            for(auto& argument: *arguments) {
                if(argument.kind() == json::Value::String) {
                    carguments.emplace_back(saver.save(*argument.getAsString()).data());
                }
            }

            auto info = this->update_command(*directory, source, carguments);
            if(info.kind != UpdateKind::Unchange) {
                infos.emplace_back(info);
            }
        } else if(auto command = object.getString("command")) {
            auto info = this->update_command(*directory, source, *command);
            if(info.kind != UpdateKind::Unchange) {
                infos.emplace_back(info);
            }
        }
    }

    return infos;
}

auto CompilationDatabase::load_compile_database(llvm::ArrayRef<std::string> compile_commands_dirs,
                                                llvm::StringRef workspace) -> void {
    auto try_load = [this, workspace](llvm::StringRef dir) {
        std::string filepath = path::join(dir, "compile_commands.json");
        auto content = fs::read(filepath);
        if(!content) {
            LOGGING_WARN("Failed to read CDB file: {}, {}", filepath, content.error());
            return false;
        }

        auto load = this->load_commands(*content, workspace);
        if(!load) {
            LOGGING_WARN("Failed to load CDB file: {}. {}", filepath, load.error());
            return false;
        }

        LOGGING_INFO("Load CDB file: {} successfully, {} items loaded", filepath, load->size());
        return true;
    };

    if(std::ranges::any_of(compile_commands_dirs, try_load)) {
        return;
    }

    LOGGING_WARN(
        "Can not found any valid CDB file from given directories, search recursively from workspace: {} ...",
        workspace);

    return;

    /// FIXME: Recursive workspace scanning shouldn't be enabled by default, as it
    /// might scan unintended directories.
    std::error_code ec;
    for(fs::recursive_directory_iterator it(workspace, ec), end; it != end && !ec;
        it.increment(ec)) {
        auto status = it->status();
        if(!status) {
            continue;
        }

        // Skip hidden directories.
        llvm::StringRef filename = path::filename(it->path());
        if(fs::is_directory(*status) && filename.starts_with('.')) {
            it.no_push();
            continue;
        }

        if(fs::is_regular_file(*status) && filename == "compile_commands.json") {
            if(try_load(path::parent_path(it->path()))) {
                return;
            }
        }
    }

    /// TODO: Add a default command in clice.toml. Or load commands from .clangd ?
    LOGGING_WARN(
        "Can not found any valid CDB file in current workspace, fallback to default mode.");
}

auto CompilationDatabase::lookup(llvm::StringRef file, CommandOptions options) -> LookupInfo {
    LookupInfo info;

    file = self->save_string(file);
    auto it = self->command_infos.find(file.data());
    if(it != self->command_infos.end()) {
        info.directory = it->second.directory;
        info.arguments = self->process_command(file, it->second, options);
    } else {
        info = self->guess_or_fallback(file);
    }

    auto record = [&info, this](llvm::StringRef argument) {
        info.arguments.emplace_back(self->save_string(argument).data());
    };

    if(options.query_driver) {
        llvm::StringRef driver = info.arguments[0];
        /// FIXME: We may want to query with current includes, because some options
        /// will affect the driver, e.g. --sysroot.
        if(auto driver_info = this->query_driver(driver)) {
            /// FIXME: Cache query result to avoid duplicate query.
            record("-nostdlibinc");

            if(!driver_info->target.empty()) {
                record(std::format("--target={}", driver_info->target));
            }

            /// FIXME: Cache -I so that we can append directly, avoid duplicate lookup.
            for(auto& system_header: driver_info->system_includes) {
                record("-isystem");
                record(system_header);
            }
        } else if(!options.suppress_logging) {
            LOGGING_WARN("Failed to query driver:{}, error:{}", driver, driver_info.error());
        }
    }

    if(options.resource_dir) {
        record(std::format("-resource-dir={}", fs::resource_dir));
    }

    info.arguments.emplace_back(file.data());
    /// TODO: apply rules in clice.toml.
    return info;
}

std::vector<const char*> CompilationDatabase::files() {
    std::vector<const char*> result;
    for(auto& [file, _]: self->command_infos) {
        result.emplace_back(file);
    }
    return result;
}

}  // namespace clice
