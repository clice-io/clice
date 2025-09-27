#include "Compiler/Command.h"
#include "Compiler/Compilation.h"
#include "Support/FileSystem.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Program.h"
#include "clang/Driver/Driver.h"
#include "Support/Logging.h"

namespace clice {

CompilationDatabase::CompilationDatabase() {
    using opions = clang::driver::options::ID;

    /// Remove the input file, we will add input file ourselves.
    filtered_options.insert(opions::OPT_INPUT);

    /// -c and -o are meaningless for frontend.
    filtered_options.insert(opions::OPT_c);
    filtered_options.insert(opions::OPT_o);
    filtered_options.insert(opions::OPT_dxc_Fc);
    filtered_options.insert(opions::OPT_dxc_Fo);

    /// Remove all options related to PCH building.
    filtered_options.insert(opions::OPT_emit_pch);
    filtered_options.insert(opions::OPT_include_pch);
    filtered_options.insert(opions::OPT__SLASH_Yu);
    filtered_options.insert(opions::OPT__SLASH_Fp);

    /// Remove all options related to C++ module, we will
    /// build module and set deps ourselves.
    filtered_options.insert(opions::OPT_fmodule_file);
    filtered_options.insert(opions::OPT_fmodule_output);
    filtered_options.insert(opions::OPT_fprebuilt_module_path);
}

auto CompilationDatabase::save_string(this Self& self, llvm::StringRef string) -> llvm::StringRef {
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

auto CompilationDatabase::save_cstring_list(this Self& self, llvm::ArrayRef<const char*> arguments)
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

namespace {

llvm::SmallVector<llvm::StringRef, 4> driver_invocation_argv(llvm::StringRef driver) {
    /// FIXME: MSVC command:` cl /Bv`, should we support it?
    /// if (driver.starts_with("gcc") || driver.starts_with("g++") || driver.starts_with("clang")) {
    ///      return {"-E", "-v", "-xc++", "/dev/null"};
    /// } else if (driver.starts_with("cl") || driver.starts_with("clang-cl")) {
    ///      return {"/Bv"};
    /// }
#if defined(_WIN32)
    const llvm::StringRef null_device = "NUL";
#else
    const llvm::StringRef null_device = "/dev/null";
#endif
    return {driver, "-E", "-v", "-xc++", null_device};
}

llvm::SmallVector<llvm::StringRef, 1> driver_invocation_env() {
#if defined(_WIN32)
    /// TODO: windows support
    return {};
#else
    /// Ensure driver print infomation in English
    return {"LANG=C"};
#endif
}

using QueryDriverError = CompilationDatabase::QueryDriverError;
using ErrorKind = CompilationDatabase::QueryDriverError::ErrorKind;

auto unexpected(ErrorKind kind, std::string message) {
    return std::unexpected<QueryDriverError>({kind, std::move(message)});
};

}  // namespace

auto CompilationDatabase::query_driver(this Self& self, llvm::StringRef driver)
    -> std::expected<DriverInfo, QueryDriverError> {
    {
        /// FIXME: Should we use a better way?
        llvm::SmallString<128> absolute_path;
        if(auto error = fs::real_path(driver, absolute_path)) {
            auto result = llvm::sys::findProgramByName(driver);
            if(!result) {
                return unexpected(ErrorKind::NotFoundInPATH, result.getError().message());
            }
            absolute_path = *result;
        }

        driver = self.save_string(absolute_path);
    }

    auto it = self.driver_infos.find(driver.data());
    if(it != self.driver_infos.end()) {
        return it->second;
    }

    auto driver_name = path::filename(driver);

    llvm::SmallString<128> output_path;
    if(auto error = llvm::sys::fs::createTemporaryFile("system-includes", "clice", output_path)) {
        return unexpected(ErrorKind::FailToCreateTempFile, error.message());
    }

    // If we fail to get the driver infomation, keep the output file for user to debug.
    bool keep_output_file = true;
    auto clean_up = llvm::make_scope_exit([&output_path, &keep_output_file]() {
        if(keep_output_file) {
            logging::warn("Query driver failed, output file:{}", output_path);
            return;
        }

        if(auto errc = llvm::sys::fs::remove(output_path)) {
            logging::warn("Fail to remove temporary file: {}", errc.message());
        }
    });

    bool is_std_err = true;
    std::optional<llvm::StringRef> redirects[] = {{""}, {""}, {""}};
    redirects[is_std_err ? 2 : 1] = output_path.str();

    llvm::SmallVector argv = driver_invocation_argv(driver);
    llvm::SmallVector env = driver_invocation_env();
    std::string message;
    if(int RC = llvm::sys::ExecuteAndWait(driver,
                                          argv,
                                          env,
                                          redirects,
                                          /*SecondsToWait=*/0,
                                          /*MemoryLimit=*/0,
                                          &message)) {
        return unexpected(ErrorKind::InvokeDriverFail, std::move(message));
    }

    auto file = llvm::MemoryBuffer::getFile(output_path);
    if(!file) {
        return unexpected(ErrorKind::OutputFileNotReadable, file.getError().message());
    }

    llvm::StringRef content = file.get()->getBuffer();

    const char* TS = "Target: ";
    const char* SIS = "#include <...> search starts here:";
    const char* SIE = "End of search list.";

    llvm::SmallVector<llvm::StringRef> lines;
    content.split(lines, '\n', -1, false);

    bool in_includes_block = false;
    bool found_start_marker = false;

    llvm::StringRef target;
    llvm::SmallVector<llvm::StringRef, 8> system_includes;

    for(const auto& line_ref: lines) {
        auto line = line_ref.trim();

        if(line.starts_with(TS)) {
            line.consume_front(TS);
            target = line;
            continue;
        }

        if(line == SIS) {
            found_start_marker = true;
            in_includes_block = true;
            continue;
        }

        if(line == SIE) {
            if(in_includes_block) {
                in_includes_block = false;
            }
            continue;
        }

        if(in_includes_block) {
            system_includes.push_back(line);
        }
    }

    if(!found_start_marker) {
        return unexpected(ErrorKind::InvalidOutputFormat, "Start marker not found...");
    }

    if(in_includes_block) {
        return unexpected(ErrorKind::InvalidOutputFormat, "End marker not found...");
    }

    // Get driver information success, remove temporary file.
    keep_output_file = false;

    llvm::SmallVector<const char*, 8> includes;
    for(auto include: system_includes) {
        llvm::SmallString<64> buffer;

        /// Make sure the path is absolute, otherwise it may be
        /// "/usr/lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13", which
        /// interferes with our determination of the resource directory
        auto err = fs::real_path(include, buffer);
        include = buffer;

        /// Remove resource dir of the driver.
        if(err || include.contains("lib/gcc")) {
            continue;
        }

        includes.emplace_back(self.save_string(buffer).data());
    }

    DriverInfo info;
    info.target = self.save_string(target);
    info.system_includes = self.save_cstring_list(includes);
    self.driver_infos.try_emplace(driver.data(), info);
    return info;
}

auto CompilationDatabase::update_command(this Self& self,
                                         llvm::StringRef directory,
                                         llvm::StringRef file,
                                         llvm::ArrayRef<const char*> arguments) -> UpdateInfo {
    file = self.save_string(file);
    directory = self.save_string(directory);

    llvm::SmallVector<const char*, 16> filtered_arguments;

    /// Append
    auto add_argument = [&](llvm::StringRef argument) {
        auto saved = self.save_string(argument);
        filtered_arguments.emplace_back(saved.data());
    };

    /// Append driver sperately.
    add_argument(arguments.front());

    unsigned missing_arg_index = 0;
    unsigned missing_arg_count = 0;
    auto& table = clang::driver::getDriverOptTable();

    /// The driver should be discarded.
    auto list = table.ParseArgs(arguments.drop_front(), missing_arg_index, missing_arg_count);

    bool remove_pch = false;

    /// Append and filter useless arguments.
    for(auto arg: list.getArgs()) {
        auto& opt = arg->getOption();
        auto id = opt.getID();

        /// Filter options we don't need.
        if(self.filtered_options.contains(id)) {
            continue;
        }

        /// For arguments -I<dir>, convert directory to absolute path.
        /// i.e xmake will generate commands in this style.
        if(id == clang::driver::options::OPT_I) {
            if(arg->getNumValues() == 1) {
                add_argument("-I");
                llvm::StringRef value = arg->getValue(0);
                if(!value.empty() && !path::is_absolute(value)) {
                    add_argument(path::join(directory, value));
                } else {
                    add_argument(value);
                }
            }
            continue;
        }

        /// A workaround to remove extra PCH when cmake
        /// generate PCH flags for clang.
        if(id == clang::driver::options::OPT_Xclang) {
            if(arg->getNumValues() == 1) {
                if(remove_pch) {
                    remove_pch = false;
                    continue;
                }

                llvm::StringRef value = arg->getValue(0);
                if(value == "-include-pch") {
                    remove_pch = true;
                    continue;
                }
            }
        }

        /// Rewrite the argument to filter arguments, we basically reimplement
        /// the logic of `Arg::render` to use our allocator to allocate memory.
        switch(opt.getRenderStyle()) {
            case llvm::opt::Option::RenderValuesStyle: {
                for(auto value: arg->getValues()) {
                    add_argument(value);
                }
                break;
            }

            case llvm::opt::Option::RenderSeparateStyle: {
                add_argument(arg->getSpelling());
                for(auto value: arg->getValues()) {
                    add_argument(value);
                }
                break;
            }

            case llvm::opt::Option::RenderJoinedStyle: {
                llvm::SmallString<256> first = {arg->getSpelling(), arg->getValue(0)};
                add_argument(first);
                for(auto value: llvm::ArrayRef(arg->getValues()).drop_front()) {
                    add_argument(value);
                }
                break;
            }

            case llvm::opt::Option::RenderCommaJoinedStyle: {
                llvm::SmallString<256> buffer = arg->getSpelling();
                for(auto i = 0; i < arg->getNumValues(); i++) {
                    if(i) {
                        buffer += ',';
                    }
                    buffer += arg->getValue(i);
                }
                add_argument(buffer);
                break;
            }
        }
    }

    /// Save arguments.
    arguments = self.save_cstring_list(filtered_arguments);

    UpdateKind kind = UpdateKind::Unchange;
    CommandInfo info = {directory, arguments};
    auto [it, success] = self.command_infos.try_emplace(file.data(), info);
    if(success) {
        kind = UpdateKind::Create;
    } else {
        auto& info = it->second;
        if(info.directory.data() != directory.data() || info.arguments.data() != arguments.data()) {
            kind = UpdateKind::Update;
            info.directory = directory;
            info.arguments = arguments;
        }
    }

    return UpdateInfo{kind, file};
}

auto CompilationDatabase::update_command(this Self& self,
                                         llvm::StringRef directory,
                                         llvm::StringRef file,
                                         llvm::StringRef command) -> UpdateInfo {
    llvm::BumpPtrAllocator local;
    llvm::StringSaver saver(local);

    llvm::SmallVector<const char*, 32> arguments;
    auto [driver, _] = command.split(' ');
    driver = path::filename(driver);

    /// FIXME: Use a better to handle this.
    if(driver.starts_with("cl") || driver.starts_with("clang-cl")) {
        llvm::cl::TokenizeWindowsCommandLineFull(command, saver, arguments);
    } else {
        llvm::cl::TokenizeGNUCommandLine(command, saver, arguments);
    }

    return self.update_command(directory, file, arguments);
}

auto CompilationDatabase::load_commands(this Self& self,
                                        llvm::StringRef json_content,
                                        llvm::StringRef workspace)
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

            auto info = self.update_command(*directory, source, carguments);
            if(info.kind != UpdateKind::Unchange) {
                infos.emplace_back(info);
            }
        } else if(auto command = object.getString("command")) {
            auto info = self.update_command(*directory, source, *command);
            if(info.kind != UpdateKind::Unchange) {
                infos.emplace_back(info);
            }
        }
    }

    return infos;
}

auto CompilationDatabase::get_command(this Self& self, llvm::StringRef file, CommandOptions options)
    -> LookupInfo {
    LookupInfo info;

    file = self.save_string(file);
    auto it = self.command_infos.find(file.data());
    if(it != self.command_infos.end()) {
        info.directory = it->second.directory;
        info.arguments = it->second.arguments;
    } else {
        info = self.guess_or_fallback(file);
    }

    auto record = [&info, &self](llvm::StringRef argument) {
        info.arguments.emplace_back(self.save_string(argument).data());
    };

    if(options.query_driver) {
        llvm::StringRef driver = info.arguments[0];
        if(auto driver_info = self.query_driver(driver)) {
            record("-nostdlibinc");

            /// FIXME: Use target information here, this is useful for cross compilation.

            /// FIXME: Cache -I so that we can append directly, avoid duplicate lookup.
            for(auto& system_header: driver_info->system_includes) {
                record("-I");
                record(system_header);
            }
        } else if(!options.suppress_log) {
            logging::warn("Failed to query driver:{}, error:{}", driver, driver_info.error());
        }
    }

    if(options.resource_dir) {
        record(std::format("-resource-dir={}", fs::resource_dir));
    }

    info.arguments.emplace_back(file.data());
    /// TODO: apply rules in clice.toml.
    return info;
}

auto CompilationDatabase::guess_or_fallback(this Self& self, llvm::StringRef file) -> LookupInfo {
    // Try to guess command from other file in same directory or parent directory
    llvm::StringRef dir = path::parent_path(file);

    // Search up to 3 levels of parent directories
    int up_level = 0;
    while(!dir.empty() && up_level < 3) {
        // If any file in the directory has a command, use that command
        for(const auto& [other_file, info]: self.command_infos) {
            llvm::StringRef other = other_file;
            // Filter case that dir is /path/to/foo and there's another directory /path/to/foobar
            if(other.starts_with(dir) &&
               (other.size() == dir.size() || path::is_separator(other[dir.size()]))) {
                logging::info("Guess command for:{}, from existed file: {}", file, other_file);
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

auto CompilationDatabase::load_compile_database(this Self& self,
                                                llvm::ArrayRef<std::string> compile_commands_dirs,
                                                llvm::StringRef workspace) -> void {
    auto try_load = [&self, workspace](llvm::StringRef dir) {
        std::string filepath = path::join(dir, "compile_commands.json");
        auto content = fs::read(filepath);
        if(!content) {
            logging::warn("Failed to read CDB file: {}, {}", filepath, content.error());
            return false;
        }

        auto load = self.load_commands(*content, workspace);
        if(!load) {
            logging::warn("Failed to load CDB file: {}. {}", filepath, load.error());
            return false;
        }

        logging::info("Load CDB file: {} successfully, {} items loaded", filepath, load->size());
        return true;
    };

    if(std::ranges::any_of(compile_commands_dirs, try_load)) {
        return;
    }

    logging::warn(
        "Can not found any valid CDB file from given directories, search recursively from workspace: {} ...",
        workspace);

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
    logging::warn(
        "Can not found any valid CDB file in current workspace, fallback to default mode.");
}

}  // namespace clice
