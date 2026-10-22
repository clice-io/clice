module;

#include "modules/prelude.h"

module clice;

import :command.argument;
import :command.file_kind;
import :command.table;
import :vfs.path;

namespace clice {

namespace {

namespace ranges = std::ranges;

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
            case option::OPT_x:
                if(before_slot && arg.values.size() == 1) {
                    llvm::StringRef value = arg.values[0];
                    x_state = value == "none" ? llvm::StringRef() : value;
                }
                break;
            case option::OPT__SLASH_TC: cl_state = "c"; break;
            case option::OPT__SLASH_TP: cl_state = "c++"; break;
        }
    }
    return cl_state.empty() ? x_state : cl_state;
}

/// Whether the driver compiles C inputs as C++, as `g++` and `clang++` do
/// under any version or target affix (`x86_64-linux-gnu-g++-13`), and
/// `zig c++` — unless a `--driver-mode=`, the last one winning, says
/// otherwise.
bool cxx_driver(const Command& command) {
    if(command.subcommand) {
        return llvm::StringRef(command.subcommand) == "c++";
    }
    for(auto& arg: std::views::reverse(command.args)) {
        if(arg.opt_id == option::OPT_driver_mode && arg.values.size() == 1) {
            return llvm::StringRef(arg.values[0]) == "g++";
        }
    }
    std::string lowered = path::filename(command.driver).lower();
    llvm::StringRef name = lowered;
    name.consume_back(".exe");
    return name.rtrim("0123456789.-").ends_with("++");
}

}  // namespace

llvm::StringRef resource_dir() {
    static std::string dir = [] {
        static int anchor;
        auto exe = llvm::sys::fs::getMainExecutable("", &anchor);
        if(exe.empty()) {
            return std::string{};
        }
        return clang::GetResourcesPath(exe);
    }();
    return dir;
}

void CommandTable::set_workspace_root(CanonicalRef path) {
    root = path;
}

const Command& CommandTable::get(CommandID id) const {
    return *commands.get(static_cast<std::uint32_t>(id));
}

LocalArg CommandTable::local_arg(
    const std::expected<kota::option::ParsedArg, kota::option::ParseError>& parsed,
    llvm::ArrayRef<std::string> tokens) {
    if(!parsed || parsed->id == option::OPT_UNKNOWN || parsed->id == option::OPT_INPUT) {
        auto index = parsed ? parsed->index : parsed.error().index;
        return {.opt_id = option::OPT_UNKNOWN,
                .cls = ArgClass::Unknown,
                .spelling = strings.save(tokens[index]).data()};
    }
    LocalArg local{.opt_id = parsed->id, .alias = written_alias(*parsed)};
    for(llvm::StringRef value: parsed->values) {
        local.values.push_back(strings.save(value).data());
    }
    local.cls = classify_option(local.opt_id, local.values);
    return local;
}

CommandID CommandTable::save(Command command, llvm::ArrayRef<LocalArg> args) {
    llvm::SmallVector<Arg, 32> view;
    for(auto& arg: args) {
        view.push_back(arg.view());
    }
    command.args = view;
    auto id = commands.get(command);
    auto stored = commands.get(id);
    if(stored->args.data() == view.data()) {
        stored->args = persist(args);
    }
    return CommandID(id);
}

llvm::ArrayRef<Arg> CommandTable::persist(llvm::ArrayRef<LocalArg> args) {
    auto* persisted = allocator->Allocate<Arg>(args.size());
    for(std::size_t i = 0; i < args.size(); i += 1) {
        persisted[i] = args[i].view();
        persisted[i].values = persisted[i].values.copy(*allocator);
    }
    return {persisted, args.size()};
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
            case ArgClass::Diagnostics: render_portable(arg); break;
            case ArgClass::Unknown: append(arg.spelling); break;
            case ArgClass::Input: append("\x01input"); break;
            case ArgClass::Codegen: break;
            // The precompiled header a command reads decides what it
            // compiles, though no render passes it on.
            case ArgClass::Discarded:
                if(arg.opt_id == option::OPT_include_pch) {
                    render_portable(arg);
                }
                break;
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
    namespace types = clang::driver::types;
    auto type = types::lookupTypeForTypeSpecifier(forced_language(id).str().c_str());
    bool emits = type != types::TY_INVALID && types::onlyPrecompileType(type);
    const char* output = nullptr;
    for(auto& arg: get(id).args) {
        if(arg.opt_id == option::OPT_emit_pch) {
            emits = true;
        } else if(arg.opt_id == option::OPT_o && arg.values.size() == 1) {
            output = arg.values[0];
        }
    }
    if(!emits || !output) {
        return std::nullopt;
    }
    return Spelling::absolute(output);
}

llvm::SmallVector<Spelling, 2> CommandTable::pch_inputs(CommandID id) const {
    auto& args = get(id).args;
    auto directory = Spelling::absolute(get(id).directory);
    for(auto& arg: args) {
        if(arg.opt_id == option::OPT_working_directory && arg.values.size() == 1) {
            directory = Spelling::absolute(arg.values[0]);
        }
    }
    llvm::SmallVector<Spelling, 2> inputs;
    for(auto& arg: args) {
        if(arg.values.size() != 1) {
            continue;
        }
        if(arg.opt_id == option::OPT_include_pch) {
            inputs.push_back(Spelling::absolute(arg.values[0]));
        } else if(arg.opt_id == option::OPT_include) {
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

    // clang reads a target off its program's name in its main alone, never
    // when its library builds the invocation; the command's own --target
    // comes later and wins.
    if(auto target = name_target(command.driver); !target.empty()) {
        argv.push_back(intern("--target=" + target).data());
    }

    // Inject our resource dir if the command names none, so the embedded
    // frontend and its builtin headers stay version-matched.
    bool has_resource_dir = ranges::any_of(command.args, [](const Arg& arg) {
        return arg.opt_id == option::OPT_resource_dir;
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

    std::size_t includes = 0;
    for(auto& arg: command.args) {
        if(arg.opt_id == option::OPT_include) {
            includes += 1;
            if(includes > opts.kept_includes) {
                continue;
            }
        }
        switch(arg.cls) {
            case ArgClass::Semantic:
            case ArgClass::UserContent:
            case ArgClass::Diagnostics:
                render_driver_arg(arg, command.family, emit);
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
                    auto type = suffix_type(source);
                    llvm::StringRef driver_lang =
                        type == types::TY_INVALID ? "" : types::getTypeName(type);
                    if(driver_lang != llvm::StringRef(ref.input.value)) {
                        if(cl_mode(command.family)) {
                            if(llvm::StringRef(ref.input.value) == "c++") {
                                emit("/TP");
                            } else if(llvm::StringRef(ref.input.value) == "c") {
                                emit("/TC");
                            }
                        } else {
                            emit("-x");
                            emit(ref.input.value);
                        }
                    }
                }
                argv.push_back(source.data());
                break;
            }
            case ArgClass::Codegen:
            case ArgClass::Discarded:
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

std::vector<const char*> CommandTable::render_full(CommandID id) {
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
        if(arg.cls == ArgClass::Input) {
            continue;
        }
        render_arg(arg, emit);
    }
    return argv;
}

}  // namespace clice
