module clice;

import :command.argument;
import :command.file_kind;
import :command.table;
import :vfs.path;

namespace clice {

namespace {

/// Version salt of the persistent command identity: command hashes change
/// wholesale on schema changes even when old and new renders happen to
/// produce the same bytes.
constexpr llvm::StringRef identity_salt = "clice-cmd-v9";

/// The language selector governing the input slot, or empty. `-x` is
/// positional (applies to inputs after it, `-x none` resets); cl's global
/// /TC and /TP apply regardless of position.
llvm::StringRef language_state_at_slot(llvm::ArrayRef<Arg> args) {
    llvm::StringRef x_state, cl_state;
    bool before_slot = true;
    for(auto& arg: args) {
        if(arg.cls == ArgClass::Input) {
            before_slot = false;
            continue;
        }
        switch(arg.opt_id) {
            case clang_options::OPT_x:
                if(before_slot && arg.values.size() == 1) {
                    llvm::StringRef value = arg.values[0];
                    x_state = value == "none" ? llvm::StringRef() : value;
                }
                break;
            case clang_options::OPT__SLASH_TC: cl_state = "c"; break;
            case clang_options::OPT__SLASH_TP: cl_state = "c++"; break;
        }
    }
    return cl_state.empty() ? x_state : cl_state;
}

/// The last of the args the option `id` spells; null for none.
const Arg* last_of(llvm::ArrayRef<Arg> args, unsigned id) {
    for(auto& arg: views::reverse(args)) {
        if(arg.opt_id == id) {
            return &arg;
        }
    }
    return nullptr;
}

/// cl's `/Yc` or `/Yu` (`id`) when no `/Y-` turns it off; null otherwise.
const Arg* cl_pch_option(llvm::ArrayRef<Arg> args, unsigned id) {
    return last_of(args, clang_options::OPT__SLASH_Y_) ? nullptr : last_of(args, id);
}

/// Whether the command writes a precompiled header: it compiles its input
/// as a header (`-x c++-header`) — as CMake builds a target's precompiled
/// headers from `cmake_pch.hxx.cxx` — emits a PCH, or creates one with cl's
/// `/Yc`, and does not only check its syntax (Bazel's header parsing).
bool writes_pch(llvm::ArrayRef<Arg> args) {
    namespace types = clang::driver::types;
    auto type = types::lookupTypeForTypeSpecifier(language_state_at_slot(args).str().c_str());
    return ((type != types::TY_INVALID && types::onlyPrecompileType(type)) ||
            last_of(args, clang_options::OPT_emit_pch) ||
            cl_pch_option(args, clang_options::OPT__SLASH_Yc)) &&
           !last_of(args, clang_options::OPT_fsyntax_only);
}

/// The command's working directory: its entry's, or the one
/// -working-directory names.
Spelling working_directory(const Command& command) {
    auto* named = last_of(command.args, clang_options::OPT_working_directory);
    return Spelling::absolute(named && named->values.size() == 1 ? named->values[0]
                                                                 : command.directory);
}

/// The PCH cl's `/Yc` writes or `/Yu` reads, `through` being that option:
/// /Fp's path, `.pch` added when it names no extension, else the through
/// header's with its extension replaced — clang-cl's driver's rule
/// (Driver::GetClPchPath). None for an option naming no header without /Fp.
std::optional<Spelling> cl_pch_path(const Command& command, const Arg& through) {
    auto directory = working_directory(command);
    if(auto* fp = last_of(command.args, clang_options::OPT__SLASH_Fp);
       fp && fp->values.size() == 1) {
        std::string path = fp->values[0];
        if(path::extension(path).empty()) {
            path += ".pch";
        }
        return Spelling::absolute(Spelling(path, directory).str());
    }
    if(through.values.size() != 1 || llvm::StringRef(through.values[0]).empty()) {
        return std::nullopt;
    }
    llvm::StringRef header = through.values[0];
    auto stem = header.drop_back(path::extension(header).size());
    return Spelling::absolute(Spelling(stem.str() + ".pch", directory).str());
}

/// Whether the driver compiles C inputs as C++, as `g++` and `clang++` do
/// under any version or target affix (`x86_64-linux-gnu-g++-13`), and
/// `zig c++` — unless a `--driver-mode=`, the last one winning, says
/// otherwise.
bool cxx_driver(const Command& command) {
    if(command.subcommand) {
        return llvm::StringRef(command.subcommand) == "c++";
    }
    for(auto& arg: views::reverse(command.args)) {
        if(arg.opt_id == clang_options::OPT_driver_mode && arg.values.size() == 1) {
            return llvm::StringRef(arg.values[0]) == "g++";
        }
    }
    std::string lowered = llvm::StringRef(path::filename(command.driver)).lower();
    llvm::StringRef name = lowered;
    name.consume_back(".exe");
    return name.rtrim("0123456789.-").ends_with("++");
}

}  // namespace

llvm::StringRef resource_dir() {
    static std::string dir = [] {
        std::string exe;
        if(sys::executable_path(exe)) {
            return std::string{};
        }
        return clang::GetResourcesPath(exe);
    }();
    return dir;
}

llvm::SmallVector<bool> RenderOptions::left_out(llvm::ArrayRef<Arg> args) const {
    llvm::SmallVector<bool> result(args.size());
    for(auto [entered, position]: views::enumerate(forced_files(args))) {
        result[position] = static_cast<std::size_t>(entered) >= kept_forced;
    }
    return result;
}

BuiltinHeader builtin_header(llvm::StringRef name) {
    struct Entry {
        llvm::StringLiteral name;
        BuiltinHeader kind;
    };

    constexpr static Entry entries[] = {
#define BUILTIN_HEADER(NAME, KIND) {NAME, BuiltinHeader::KIND},
#include "builtin_headers.inc"
#undef BUILTIN_HEADER
    };
    auto found = ranges::lower_bound(entries, name, {}, [](const Entry& entry) {
        return llvm::StringRef(entry.name);
    });
    return found != ranges::end(entries) && found->name == name ? found->kind : BuiltinHeader::None;
}

void CommandTable::set_project_root(CanonicalRef path) {
    root = path;
}

const Command& CommandTable::get(CommandID id) const {
    return commands[std::to_underlying(id)];
}

std::optional<LocalArg>
    CommandTable::local_arg(const std::expected<cli::ParsedArg, cli::Error>& parsed,
                            llvm::ArrayRef<std::string> tokens) {
    if(!parsed || parsed->id == clang_options::OPT_UNKNOWN ||
       parsed->id == clang_options::OPT_INPUT) {
        auto index = parsed ? parsed->index : parsed.error().begin;
        return LocalArg{.opt_id = clang_options::OPT_UNKNOWN,
                        .cls = ArgClass::Unknown,
                        .spelling = strings.save(tokens[index]).data()};
    }
    // Where a module unit writes its BMI is the build's business; whether
    // it writes one decides the BMI a precompile writes (Toolchain::synthesize).
    if(parsed->id == clang_options::OPT_fmodule_output_EQ) {
        return LocalArg{.opt_id = clang_options::OPT_fmodule_output, .cls = ArgClass::Precompiled};
    }
    LocalArg local{.opt_id = parsed->id, .alias = written_alias(*parsed)};
    for(llvm::StringRef value: parsed->values) {
        local.values.push_back(strings.save(value).data());
    }
    auto cls = classify_option(local.opt_id, local.values);
    if(!cls) {
        return std::nullopt;
    }
    local.cls = *cls;
    return local;
}

CommandID CommandTable::save(Command command, llvm::ArrayRef<LocalArg> args) {
    llvm::SmallVector<Arg, 32> view;
    for(auto& arg: args) {
        view.push_back(arg.view());
    }
    // An output names a file of one unit: kept only to find the
    // precompiled header a command writes, it would set every other
    // command apart.
    if(!writes_pch(view)) {
        view.erase(
            ranges::remove_if(view,
                              [](const Arg& arg) { return arg.opt_id == clang_options::OPT_o; })
                .begin(),
            view.end());
    }
    command.args = view;
    if(auto it = ids.find(command); it != ids.end()) {
        return it->second;
    }
    command.args = persist(view);
    auto id = CommandID(commands.size());
    commands.push_back(command);
    ids.try_emplace(command, id);
    return id;
}

llvm::ArrayRef<Arg> CommandTable::persist(llvm::ArrayRef<Arg> args) {
    auto* persisted = allocator->Allocate<Arg>(args.size());
    for(std::size_t i = 0; i < args.size(); i += 1) {
        persisted[i] = args[i];
        persisted[i].values = persisted[i].values.copy(*allocator);
    }
    return {persisted, args.size()};
}

llvm::ArrayRef<Arg> CommandTable::persist(llvm::ArrayRef<LocalArg> args) {
    llvm::SmallVector<Arg, 32> view;
    for(auto& arg: args) {
        view.push_back(arg.view());
    }
    return persist(view);
}

llvm::StringRef CommandTable::intern(llvm::StringRef text) {
    return strings.save(text);
}

void CommandTable::render_identity(CommandID id, std::string& out) {
    auto& command = get(id);
    auto append = [&](std::string_view fragment) {
        out += fragment;
        out += '\0';
    };

    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    auto render_portable = [&](const Arg& arg) {
        llvm::SmallVector<const char*, 2> values;
        for(const char* value: arg.values) {
            llvm::SmallString<256> storage;
            auto name = path::portable(value, root, storage);
            values.push_back(name.data() == value ? value : saver.save(name).data());
        }
        render_arg(
            {.opt_id = arg.opt_id, .cls = arg.cls, .spelling = arg.spelling, .values = values},
            append);
    };

    // A compiler named by path is spelled like every other path first.
    auto driver = path::is_absolute(command.driver) ? Spelling::absolute(command.driver).str()
                                                    : std::string(command.driver);
    llvm::SmallString<256> storage;
    append(path::portable(driver, root, storage));
    if(command.subcommand) {
        append(command.subcommand);
    }
    for(auto& arg: command.args) {
        switch(arg.cls) {
            case ArgClass::Semantic:
            case ArgClass::UserContent:
            case ArgClass::Diagnostics:
            case ArgClass::Precompiled: render_portable(arg); break;
            case ArgClass::Unknown: append(arg.spelling); break;
            case ArgClass::Input: append("\x01input"); break;
        }
    }
    if(!command.query_toolchain) {
        append("\x01unqueried");
    }
}

std::uint64_t CommandTable::command_hash(CommandID id) {
    auto [it, inserted] = command_hashes.try_emplace(static_cast<std::uint32_t>(id), 0);
    if(!inserted) {
        return it->second;
    }

    std::string buf;
    buf += identity_salt;
    buf += '\0';
    render_identity(id, buf);
    llvm::SmallString<256> storage;
    buf += path::portable(get(id).directory, root, storage);
    it->second = llvm::xxh3_64bits(buf);
    return it->second;
}

std::string CommandTable::command_hash_hex(CommandID id) {
    return std::format("{:016x}", command_hash(id));
}

llvm::StringRef CommandTable::forced_language(CommandID id) const {
    return language_state_at_slot(get(id).args);
}

std::optional<Spelling> CommandTable::pch_output(CommandID id) const {
    auto& command = get(id);
    if(auto* created = cl_pch_option(command.args, clang_options::OPT__SLASH_Yc)) {
        return cl_pch_path(command, *created);
    }
    auto* output = last_of(command.args, clang_options::OPT_o);
    return output && output->values.size() == 1
               ? std::optional(Spelling::absolute(output->values[0]))
               : std::nullopt;
}

llvm::SmallVector<Spelling, 2> CommandTable::pch_inputs(CommandID id) const {
    auto& command = get(id);
    auto& args = command.args;
    auto directory = working_directory(command);
    llvm::SmallVector<Spelling, 2> inputs;
    if(auto* used = cl_pch_option(args, clang_options::OPT__SLASH_Yu)) {
        if(auto pch = cl_pch_path(command, *used)) {
            inputs.push_back(std::move(*pch));
        }
    }
    for(auto& arg: args) {
        if(arg.values.size() != 1) {
            continue;
        }
        if(arg.opt_id == clang_options::OPT_include_pch) {
            inputs.push_back(Spelling::absolute(arg.values[0]));
        } else if(arg.opt_id == clang_options::OPT_include) {
            Spelling header(arg.values[0], directory);
            inputs.push_back(Spelling::absolute(header.str() + ".pch"));
            inputs.push_back(Spelling::absolute(header.str() + ".gch"));
        }
    }
    return inputs;
}

bool CommandTable::forces_c_family(CommandID id) const {
    namespace types = clang::driver::types;
    return is_c_family_type(types::lookupTypeForTypeSpecifier(forced_language(id).str().c_str()));
}

InputKind CommandTable::input_kind(CommandID id, llvm::StringRef file) {
    auto state = language_state_at_slot(get(id).args);
    /// With no mapping the raw extension keys the query (the driver sees a
    /// temp file with the same extension, exactly as confused as it would
    /// be by the real file).
    auto language = state.empty() ? extension_language(file, cxx_driver(get(id))) : state;
    return {strings.save(language).data()};
}

std::vector<const char*> CommandTable::render_driver(const CommandRef& ref,
                                                     llvm::StringRef path,
                                                     const RenderOptions& opts) {
    auto& command = get(ref.command);
    auto source = intern(path);

    std::vector<const char*> argv;
    argv.reserve(command.args.size() + 8);
    argv.push_back(command.driver);
    if(command.subcommand) {
        argv.push_back(command.subcommand);
    }

    // Inject our resource dir if the command names none, so the embedded
    // frontend and its builtin headers stay version-matched.
    // LIMITATION: a runtime's headers written for an older clang's predefines
    // and wrapper headers fail: ROCm 5.7's HIP headers (Ubuntu 24.04's
    // packages) use `__AMDGCN_WAVEFRONT_SIZE` and `__ocml_*_f64`, which this
    // clang neither predefines nor declares, so its HIP units show errors in
    // ROCm's headers; ROCm 7.2 is clean.
    bool has_resource_dir = ranges::any_of(command.args, [](const Arg& arg) {
        return arg.opt_id == clang_options::OPT_resource_dir;
    });
    if(!has_resource_dir && !resource_dir().empty()) {
        argv.push_back("-resource-dir");
        argv.push_back(resource_dir().data());
    }

    auto emit = [&](llvm::StringRef fragment) {
        argv.push_back(strings.save(fragment).data());
    };

    auto state = language_state_at_slot(command.args);
    std::size_t last_user_content = argv.size();

    auto left_out = opts.left_out(command.args);
    for(auto [i, arg]: views::enumerate(command.args)) {
        if(left_out[i]) {
            continue;
        }
        switch(arg.cls) {
            case ArgClass::Semantic:
            case ArgClass::UserContent:
            case ArgClass::Diagnostics:
                render_driver_arg(arg, option_family(command), emit);
                if(arg.cls == ArgClass::UserContent) {
                    last_user_content = argv.size();
                }
                break;
            case ArgClass::Input: {
                /// The slot contract: with no governing selector and an
                /// extension the driver would classify differently from
                /// the ref's language (a borrowed header, a driver-unknown
                /// extension), an explicit selector precedes the file.
                if(state.empty()) {
                    namespace types = clang::driver::types;
                    auto type = driver_type(source);
                    llvm::StringRef driver_lang =
                        type == types::TY_INVALID ? "" : types::getTypeName(type);
                    llvm::StringRef language(ref.language.value);
                    bool cl = cl_mode(option_family(command));
                    if(driver_lang != language) {
                        if(cl && language == "c++") {
                            emit("/TP");
                        } else if(cl && language == "c") {
                            emit("/TC");
                        } else if(!cl || language == "cuda") {
                            // cl has no selector of CUDA, which an nvcc
                            // command with a cl host compiles: clang's -x
                            // is one of cl mode's options too.
                            emit("-x");
                            emit(language);
                        }
                    }
                }
                argv.push_back(source.data());
                break;
            }
            case ArgClass::Precompiled:
            case ArgClass::Unknown: break;
        }
    }

    if(opts.preamble) {
        /// After the command's own user-content flags: the host's -include
        /// runs before the synthesized preamble.
        argv.insert(argv.begin() + last_user_content, {"-include", opts.preamble});
    }

    return argv;
}

std::vector<const char*> CommandTable::render_full(CommandID id, llvm::StringRef input) {
    auto& command = get(id);
    std::vector<const char*> argv;
    argv.reserve(command.args.size() + 2);
    argv.push_back(command.driver);
    if(command.subcommand) {
        argv.push_back(command.subcommand);
    }
    auto emit = [&](llvm::StringRef fragment) {
        argv.push_back(strings.save(fragment).data());
    };
    for(auto& arg: command.args) {
        if(arg.cls != ArgClass::Input) {
            render_arg(arg, emit);
        } else if(!input.empty()) {
            emit(input);
        }
    }
    return argv;
}

}  // namespace clice
