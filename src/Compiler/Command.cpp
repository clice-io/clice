#include "Compiler/Command.h"
#include "Compiler/Compilation.h"
#include "Support/FileSystem.h"
#include "Support/Logging.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Program.h"
#include "clang/Driver/Driver.h"

namespace clice {

namespace {

bool enable_dash_dash_parsing(const llvm::opt::OptTable& table);

bool enable_grouped_short_options(const llvm::opt::OptTable& table);

template <auto MP1, auto MP2>
struct Thief {
    friend bool enable_dash_dash_parsing(const llvm::opt::OptTable& table) {
        return table.*MP1;
    }

    friend bool enable_grouped_short_options(const llvm::opt::OptTable& table) {
        return table.*MP2;
    }
};

template struct Thief<&llvm::opt::OptTable::DashDashParsing,
                      &llvm::opt::OptTable::GroupedShortOptions>;

class ArgumentParser final : public llvm::opt::ArgList {
public:
    ArgumentParser(llvm::BumpPtrAllocator& allocator) : allocator(allocator) {}

    ~ArgumentParser() {
        /// We never use the private `Args` field, so make sure it's empty.
        if(getArgs().size() != 0) {
            std::abort();
        }
    }

    const char* getArgString(unsigned index) const override {
        return arguments[index];
    }

    unsigned getNumInputArgStrings() const override {
        return arguments.size();
    }

    const char* MakeArgStringRef(llvm::StringRef s) const override {
        auto p = allocator.Allocate<char>(s.size() + 1);
        std::ranges::copy(s, p);
        p[s.size()] = '\0';
        return p;
    }

    inline static auto& option_table = clang::driver::getDriverOptTable();

    void set_arguments(llvm::ArrayRef<const char*> arguments) {
        if(getArgs().size() != 0) {
            std::abort();
        }

        this->arguments = arguments;
    }

    std::unique_ptr<llvm::opt::Arg> parse_one(unsigned& index) {
        assert(!enable_dash_dash_parsing(option_table));
        assert(!enable_grouped_short_options(option_table));
        return option_table.ParseOneArg(*this, index);
    }

    void parse(llvm::ArrayRef<const char*> arguments, const auto& on_parse, const auto& on_error) {
        this->arguments = arguments;

        unsigned it = 0;
        while(it != arguments.size()) {
            llvm::StringRef s = arguments[it];

            if(s.empty()) [[unlikely]] {
                it += 1;
                continue;
            }

            auto prev = it;
            auto arg = parse_one(it);
            assert(it > prev && "parser failed to consume argument");

            if(!arg) [[unlikely]] {
                assert(it >= arguments.size() && "unexpected parser error!");
                assert(it - prev - 1 && "no missing arguments!");
                on_error(prev, it - prev - 1);
                break;
            }

            on_parse(std::move(arg));
        }
    }

private:
    llvm::ArrayRef<const char*> arguments;

    llvm::BumpPtrAllocator& allocator;
};

using QueryDriverError = CompilationDatabase::QueryDriverError;
using ErrorKind = CompilationDatabase::QueryDriverError::ErrorKind;
using options = clang::driver::options::ID;

auto unexpected(ErrorKind kind, std::string message) {
    return std::unexpected<QueryDriverError>({kind, std::move(message)});
};

}  // namespace

CompilationDatabase::CompilationDatabase() {

    /// Remove the input file, we will add input file ourselves.
    filtered_options.insert(options::OPT_INPUT);

    /// -c and -o are meaningless for frontend.
    filtered_options.insert(options::OPT_c);
    filtered_options.insert(options::OPT_o);
    filtered_options.insert(options::OPT_dxc_Fc);
    filtered_options.insert(options::OPT_dxc_Fo);

    /// Remove all options related to PCH building.
    filtered_options.insert(options::OPT_emit_pch);
    filtered_options.insert(options::OPT_include_pch);
    filtered_options.insert(options::OPT__SLASH_Yu);
    filtered_options.insert(options::OPT__SLASH_Fp);

    /// Remove all options related to C++ module, we will
    /// build module and set deps ourselves.
    filtered_options.insert(options::OPT_fmodule_file);
    filtered_options.insert(options::OPT_fmodule_output);
    filtered_options.insert(options::OPT_fprebuilt_module_path);

    parser = new ArgumentParser(allocator);
}

CompilationDatabase::CompilationDatabase(CompilationDatabase&& other) :
    parser(other.parser), allocator(std::move(other.allocator)),
    string_cache(std::move(other.string_cache)), arguments_cache(std::move(other.arguments_cache)),
    filtered_options(std::move(other.filtered_options)),
    command_infos(std::move(other.command_infos)), driver_infos(std::move(other.driver_infos)) {
    other.parser = nullptr;
}

CompilationDatabase& CompilationDatabase::operator= (CompilationDatabase&& other) {
    delete static_cast<ArgumentParser*>(parser);
    parser = other.parser;
    other.parser = nullptr;

    allocator = std::move(other.allocator);
    string_cache = std::move(other.string_cache);
    arguments_cache = std::move(other.arguments_cache);
    filtered_options = std::move(other.filtered_options);
    command_infos = std::move(other.command_infos);
    driver_infos = std::move(other.driver_infos);

    return *this;
}

CompilationDatabase::~CompilationDatabase() {
    delete static_cast<ArgumentParser*>(parser);
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

    /// FIXME: Is it possible that the output is not in stderr?
    std::optional<llvm::StringRef> redirects[3] = {
        {""},
        {""},
        {output_path.str()},
    };

    /// If the env is `std::nullopt`, `ExecuteAndWait` will inherit env from parent process,
    /// which is very important for msvc and clang on windows. Thay depend on the environment
    /// variables to find correct standard library path.
    constexpr auto env = std::nullopt;

#ifdef _WIN32
    llvm::SmallVector<llvm::StringRef> argv;
    if(driver_name.starts_with("cl") || driver_name.starts_with("clang-cl")) {
        /// FIXME: MSVC command:` cl /Bv`, should we support it?
        return unexpected(ErrorKind::InvokeDriverFail,
                          std::format("Unsupported driver: {}", driver));
    } else {
        argv = {driver, "-E", "-v", "-xc++", "NUL"};
    }
#else
    /// FIXME: We should find a better way to convert "LANG=C", this is important
    /// for gcc with locality. Otherwise, it will output non-ASCII char.
    llvm::SmallVector<llvm::StringRef> argv = {"LANG=C", driver, "-E", "-v", "-xc++", "/dev/null"};
#endif

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
        if(err ||
           include.contains("lib/gcc")
           /// FIXME: Only for windows, for Mac removing default resource dir
           /// may result in unexpected error. Figure out it.
           || include.contains("lib\\clang")) {
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
    auto parser = static_cast<ArgumentParser*>(self.parser);
    parser->set_arguments(arguments);

    file = self.save_string(file);
    directory = self.save_string(directory);

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
        /// command, and processing it this way would lead to its incorrect removal. To fix these
        /// corner cases, it's necessary to parse the command line fully. Additionally, detailed
        /// benchmarks should be conducted to determine the time required for parsing command-line
        /// arguments in order to decide if it's worth doing so.
        if(ranges::any_of(output_options,
                          [&](llvm::StringRef option) { return argument.starts_with(option); })) {
            auto prev = it;
            auto arg = parser->parse_one(it);

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
                logging::warn(
                    "clice currently supports only one response file in the command, when loads {}",
                    file);
            }
            response_file = self.save_string(argument);
            response_file_index = it;
            continue;
        }

        canonical_arguments.push_back(self.save_string(argument).data());
    }

    /// Cache the canonical arguments
    arguments = self.save_cstring_list(canonical_arguments);

    UpdateKind kind = UpdateKind::Unchange;
    CommandInfo info = {
        directory,
        arguments,
        response_file,
        response_file_index,
    };

    auto [it, success] = self.command_infos.try_emplace(file.data(), info);
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

auto CompilationDatabase::process_command(this Self& self,
                                          llvm::StringRef file,
                                          const CommandInfo& info,
                                          const CommandOptions& options)
    -> std::vector<const char*> {

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
    auto parser = static_cast<ArgumentParser*>(self.parser);
    auto on_error = [&](int index, int count) {
        logging::warn("missing argument index: {}, count: {} when parse: {}", index, count, file);
    };

    /// Prepare for removing arguments.
    llvm::SmallVector<const char*> remove;
    for(auto& arg: options.remove) {
        remove.push_back(self.save_string(arg).data());
    }

    /// FIXME: Handle unknow remove arguments.
    llvm::SmallVector<Arg> known_remove_args;
    parser->parse(
        remove,
        [&known_remove_args](Arg arg) { known_remove_args.emplace_back(std::move(arg)); },
        on_error);
    auto get_id = [](const Arg& arg) {
        return arg->getOption().getID();
    };
    ranges::sort(known_remove_args, {}, get_id);

    bool remove_pch = false;

    /// FIXME: Append the commands from response file.
    parser->parse(
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

                /// Compare each value, convert `const char*` to `llvm::StringRef` for comparing.
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

auto CompilationDatabase::get_command(this Self& self, llvm::StringRef file, CommandOptions options)
    -> LookupInfo {
    LookupInfo info;

    file = self.save_string(file);
    auto it = self.command_infos.find(file.data());
    if(it != self.command_infos.end()) {
        info.directory = it->second.directory;
        info.arguments = self.process_command(file, it->second, options);
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
        } else if(!options.suppress_logging) {
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
