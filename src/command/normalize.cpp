module clice;

import :command.argument;
import :command.normalize;
import :command.table;
import :command.toolchains.gcc;
import :command.toolchains.nvcc;
import :command.toolchains.own_options;
import :support.logging;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

bool is_wrapper_name(llvm::StringRef filename) {
    /// Windows tools emit spellings like CCACHE.EXE — match lowercased.
    std::string lowered = filename.lower();
    llvm::StringRef name = lowered;
    name.consume_back(".exe");
    return name == "ccache" || name == "sccache" || name == "distcc" || name == "icecc";
}

/// KEY=VAL-shaped token — a wrapper option's separate value (`ccache
/// --set-config max_size=1G`), never the compiler: the key admits only
/// [A-Za-z0-9_], which no driver path satisfies up to an '='.
bool is_assignment_token(llvm::StringRef token) {
    auto eq = token.find('=');
    if(eq == llvm::StringRef::npos || eq == 0) {
        return false;
    }
    return ranges::all_of(token.take_front(eq),
                          [](char c) { return llvm::isAlnum(c) || c == '_'; });
}

/// `family` as the `--driver-mode=` options among `tokens` leave it, the
/// last one winning.
template <typename Tokens>
CompilerFamily with_driver_mode(CompilerFamily family,
                                llvm::StringRef driver,
                                const Tokens& tokens) {
    for(llvm::StringRef token: tokens) {
        if(token.consume_front("--driver-mode=")) {
            family = token == "cl" ? CompilerFamily::ClangCL : driver_family(driver);
        }
    }
    return family;
}

/// Whether `option`, read with the options `visibility` names, takes `next`
/// as its value: `-o out.o`, `-include config.h`, `-x c++`; as well when
/// the two alone do not parse.
bool takes_next(llvm::StringRef option, llvm::StringRef next, unsigned visibility) {
    std::array<std::string_view, 2> pair = {option, next};
    auto options = cli::ParseOptions{.dash_dash_parsing = true, .visibility = visibility};
    for(auto& parsed: clang_options::table().parse(pair, options)) {
        return !parsed || parsed->next_index > 1;
    }
    return false;
}

using ParsedArgs = std::vector<std::expected<cli::ParsedArg, cli::Error>>;

/// `staged` with what its options `through` pass on — clang-cl's `/clang:`
/// to clang's driver, `-Xclang` to cc1 — read by the options `visibility`
/// names: the driver passes all their values on together, in order. The
/// command keeps them as written but for an option clice manages itself or
/// the compile never takes, which goes as its own spelling does
/// (classify_option). CMake passes a target's precompiled header to cc1
/// directly, `-Xclang -include-pch -Xclang <pch>` in its units' commands,
/// `-Xclang -emit-pch` in the one building it: the file on disk may come
/// from GCC or another clang, so a unit loads clice's own build of it
/// instead (Project::pch_writer). Bazel's module rules (xclang's) embed a
/// module's sources in it (`-fmodules-embed-all-files`). Unreal's clang-cl
/// commands write a dependency file each through `/clang:-MF<file>`.
ParsedArgs
    read_passed(ParsedArgs staged, CommandTable& commands, unsigned through, unsigned visibility) {
    llvm::SmallVector<std::size_t> passing;
    llvm::SmallVector<std::string_view> passed;
    for(std::size_t i = 0; i < staged.size(); i += 1) {
        if(staged[i] && staged[i]->id == through) {
            passing.push_back(i);
            passed.push_back(staged[i]->values[0]);
        }
    }
    // The option an argument reads as in place of the options passing it
    // on it starts, which go.
    std::vector<std::optional<cli::ParsedArg>> read(staged.size());
    std::vector<bool> gone(staged.size());
    for(auto& option: clang_options::table().parse(passed, {.visibility = visibility})) {
        // An option missing its value, which only the last can be.
        if(!option) {
            break;
        }
        if(option->id == clang_options::OPT_UNKNOWN || option->id == clang_options::OPT_INPUT) {
            continue;
        }
        llvm::SmallVector<const char*> values;
        for(auto value: option->values) {
            values.push_back(commands.intern(value).data());
        }
        auto cls = classify_option(option->id, values);
        if(cls && cls != ArgClass::Precompiled) {
            continue;
        }
        for(auto k = option->index; k < option->next_index; k += 1) {
            gone[passing[k]] = true;
        }
        read[passing[option->index]] = std::move(*option);
    }
    ParsedArgs result;
    for(std::size_t i = 0; i < staged.size(); i += 1) {
        if(read[i]) {
            result.push_back(std::move(*read[i]));
        } else if(!gone[i]) {
            result.push_back(std::move(staged[i]));
        }
    }
    return result;
}

}  // namespace

Spelling spelled(llvm::StringRef text, const Spelling& base, const fs::cygwin::MountTable* mounts) {
    if(mounts && fs::cygwin::is_posix_path(text)) {
        std::string windows;
        mounts->to_windows(windows, text);
        return Spelling::absolute(windows);
    }
    return Spelling(text, base);
}

llvm::StringRef anchored(CommandTable& commands,
                         unsigned id,
                         llvm::StringRef value,
                         const Spelling& anchor,
                         const fs::cygwin::MountTable* mounts) {
    // A forced include names a file too, which clang looks for rather than
    // anchors when relative.
    if(mounts && fs::cygwin::is_posix_path(value) &&
       (id == clang_options::OPT_include || id == clang_options::OPT_imacros)) {
        std::string windows;
        mounts->to_windows(windows, value);
        return commands.intern(windows);
    }
    if(!names_path(id, value)) {
        return value;
    }
    return commands.intern(spelled(value, anchor, mounts).str());
}

std::size_t wrapper_prefix_len(kotatsu::string_list_ref argv) {
    std::size_t i = 0;
    while(i < argv.size() && is_wrapper_name(path::filename(argv[i]))) {
        i += 1;
        while(i < argv.size() &&
              (llvm::StringRef(argv[i]).starts_with("-") || is_assignment_token(argv[i]))) {
            i += 1;
        }
    }
    return i;
}

std::optional<CommandID> Normalizer::normalize(const Spelling& directory,
                                               Fid file,
                                               kotatsu::string_list_ref command_line) {
    if(command_line.empty()) {
        return std::nullopt;
    }

    /// Wrapper stripping: the prefix is entry provenance, not command
    /// identity — `ccache clang++ X` and `clang++ X` dedupe to one command.
    std::size_t wrapper_len = wrapper_prefix_len(command_line);
    if(wrapper_len >= command_line.size()) {
        LOG_WARN("Compiler launcher without a compiler: {}", join_command(command_line));
        return std::nullopt;
    }
    llvm::SmallVector<std::string_view, 32> written(command_line.begin() + wrapper_len,
                                                    command_line.end());
    if(written.front().empty()) {
        return std::nullopt;
    }
    llvm::ArrayRef<std::string_view> arguments = written;

    Command command;
    command.directory = commands.intern(directory).data();
    llvm::StringRef driver = arguments[0];
    std::string windows;
    if(mounts && fs::cygwin::is_posix_path(driver)) {
        mounts->to_windows(windows, driver);
        driver = windows;
    }
    command.driver = commands.intern(driver).data();
    arguments = arguments.drop_front();

    command.family = driver_family(command.driver);

    /// zig cc / zig c++: the two tokens together are the driver identity.
    if(command.family == CompilerFamily::Zig && !arguments.empty() &&
       (arguments[0] == "cc" || arguments[0] == "c++")) {
        command.subcommand = commands.intern(arguments[0]).data();
        arguments = arguments.drop_front();
    }
    /// --driver-mode may also arrive from inside a response file (clang
    /// interprets it post-expansion); the pre-expansion scan only picks the
    /// response tokenization style.
    command.family = with_driver_mode(command.family, command.driver, arguments);

    auto parse_args = expand_response_files(arguments, directory, command.family);
    command.family = with_driver_mode(command.family, command.driver, parse_args);

    /// ccache's --ccache-skip guards the NEXT token from ccache's own
    /// processing; the token itself belongs to the compiler command.
    if(wrapper_len > 0) {
        std::erase(parse_args, "--ccache-skip");
    }

    /// nvcc spellings are rewritten into clang's before the table parse —
    /// the table cannot parse them, and unparsed tokens keep no semantics.
    if(command.family == CompilerFamily::NVCC) {
        command.host = nvcc_host(parse_args);
        llvm::SmallVector<const char*, 32> argv = {command.driver};
        for(auto& argument: parse_args) {
            argv.push_back(argument.c_str());
        }
        parse_args = translate_nvcc_command(argv, directory, command.host);
        parse_args.erase(parse_args.begin());
    }

    auto family = option_family(command);
    auto parse_options =
        cli::ParseOptions{.dash_dash_parsing = true, .visibility = family_visibility(family)};

    /// Two passes: the staging pass keeps every parse result alive, so the
    /// classification pass can decide input pairing (which /Tc-/Tp names
    /// the entry file) before it lays down arguments.
    auto staged = read_passed(parse_arguments(parse_args, family, parse_options),
                              commands,
                              clang_options::OPT_Xclang,
                              clang_options::CC1Option);
    staged = read_passed(std::move(staged),
                         commands,
                         clang_options::OPT__SLASH_clang,
                         family_visibility(CompilerFamily::Clang));

    /// Relative paths of the command resolve where the compile runs: the
    /// entry directory, moved by `-working-directory`.
    auto anchor = directory;
    for(auto& parsed: staged) {
        if(parsed && parsed->id == clang_options::OPT_working_directory &&
           parsed->values.size() == 1) {
            anchor = spelled(parsed->values[0], directory, mounts);
        }
    }

    /// Does this token name the entry's file? Compared by identity.
    auto matches_entry = [&](llvm::StringRef token) {
        return file.valid() && !token.empty() && files.find(spelled(token, anchor, mounts)) == file;
    };

    /// A per-file selector naming the entry file forces its language and
    /// suppresses any global /TC-/TP (cl gives per-file precedence).
    std::optional<unsigned> forced_selector;
    for(auto& parsed: staged) {
        if(!parsed.has_value()) {
            continue;
        }
        auto id = parsed->id;
        if((id == clang_options::OPT__SLASH_Tc || id == clang_options::OPT__SLASH_Tp) &&
           parsed->values.size() == 1 && matches_entry(parsed->values[0])) {
            forced_selector = id == clang_options::OPT__SLASH_Tc ? clang_options::OPT__SLASH_TC
                                                                 : clang_options::OPT__SLASH_TP;
        }
    }

    // GCC's -fmodules is its C++20 modules (flags_for_clang).
    bool header_modules = false;
    if(family != CompilerFamily::GCC) {
        for(auto& parsed: staged) {
            if(parsed && (parsed->id == clang_options::OPT_fmodules ||
                          parsed->id == clang_options::OPT_fno_modules)) {
                header_modules = parsed->id == clang_options::OPT_fmodules;
            }
        }
    }

    std::vector<LocalArg> args;
    args.reserve(staged.size() + 1);
    bool slot_placed = false;

    auto place_slot = [&] {
        args.push_back({.opt_id = clang_options::OPT_INPUT, .cls = ArgClass::Input});
        slot_placed = true;
    };

    for(auto& parsed: staged) {
        auto id = parsed ? parsed->id : clang_options::OPT_UNKNOWN;

        /// The entry's own input token becomes the slot, preserving its
        /// position (language selectors before it govern it). Other inputs
        /// — nvcc query leftovers aside, a multi-input command — drop,
        /// paired with their per-file selectors. Input args carry their
        /// token as the spelling.
        ///
        /// NVCC is the exception: the translation already resolved every
        /// positional semantic (nvcc options are command-wide last-wins)
        /// and parks accumulated state after the original input's spot —
        /// the slot goes to the end so edits keep landing after it.
        if(id == clang_options::OPT_INPUT) {
            llvm::StringRef token = parsed->values.empty() ? llvm::StringRef(parsed->spelling)
                                                           : llvm::StringRef(parsed->values[0]);
            // CMake's module map (`<object>.modmap`) holds module options
            // alone, which clice drops for its own module builds; the
            // build's scan writes it, so a project not built yet lacks it.
            if(token.starts_with("@") && unreadable && !token.ends_with(".modmap")) {
                unreadable->push_back(spelled(token.drop_front(), directory, mounts).str());
            }
            if(command.family == CompilerFamily::NVCC) {
                continue;
            }
            if(!slot_placed && matches_entry(token)) {
                place_slot();
            }
            continue;
        }

        if(id == clang_options::OPT__SLASH_Tc || id == clang_options::OPT__SLASH_Tp) {
            if(!slot_placed && parsed->values.size() == 1 && matches_entry(parsed->values[0])) {
                /// Rewritten to the equivalent global selector: a slot
                /// attribute keyed by the file would break the
                /// (command, edit set) memo — the same command can serve
                /// entries with different per-file selector values.
                args.push_back({.opt_id = *forced_selector, .cls = ArgClass::Semantic});
                place_slot();
            }
            continue;
        }

        if((id == clang_options::OPT__SLASH_TC || id == clang_options::OPT__SLASH_TP) &&
           forced_selector) {
            continue;
        }

        // LIMITATION: Clang's header modules are dropped: a file compiles
        // without them, so its `@import`s are errors. Projects whose
        // commands carry -fmodules reach it.
        if(header_modules && id == clang_options::OPT_fmodules) {
            static std::once_flag reported;
            std::call_once(reported, [] {
                LOG_GUIDANCE(
                    "The compile commands use Clang header modules (-fmodules), which clice does "
                    "not support: their files compile without them, and an `@import` is an error");
            });
            continue;
        }
        // The module files of a header module build are its header modules
        // (Bazel's `-fmodule-file=<target>.pcm`), written by the build's
        // clang, not C++20 header units.
        if(header_modules && id == clang_options::OPT_fmodule_file) {
            continue;
        }

        auto local = commands.local_arg(parsed, parse_args);
        if(!local) {
            continue;
        }

        /// g++ knows warnings clang does not (-Wlogical-op, -Wuseless-cast),
        /// each of which clang would report as unknown on every file: in its
        /// commands, and in what nvcc passes it.
        if((family == CompilerFamily::GCC || command.host == CompilerFamily::GCC) &&
           id == clang_options::OPT_W_Joined && !clang_warning(local->values[0])) {
            continue;
        }

        /// Path values absolutize where the compile runs, so the command
        /// keeps meaning when consumed away from it: the toolchain query
        /// runs elsewhere.
        // The working directory itself is relative to the entry's.
        bool working = id == clang_options::OPT_working_directory;
        for(auto& value: local->values) {
            value = anchored(commands, id, value, working ? directory : anchor, mounts).data();
        }

        args.push_back(std::move(*local));
    }

    if(!slot_placed) {
        place_slot();
    }
    return commands.save(command, args);
}

std::optional<CommandID> Normalizer::normalize(const Spelling& directory,
                                               Fid file,
                                               llvm::StringRef command) {
    llvm::SmallVector<std::string_view> views;
    std::vector<std::string> tokens;
    return normalize(directory, file, split(command, mounts != nullptr, views, tokens));
}

kotatsu::string_list_ref Normalizer::split(llvm::StringRef command,
                                           bool posix,
                                           llvm::SmallVectorImpl<std::string_view>& views,
                                           std::vector<std::string>& tokens) {
    auto quoting = posix ? sys::cmd::Quoting::POSIX : sys::cmd::native_quoting;
    views.clear();
    // Blanks before the program are none of the command (split_command).
    for(auto it = sys::cmd::split(command.ltrim(), quoting).begin(); it != std::default_sentinel;
        ++it) {
        if(!it.is_view()) {
            split_command(command, quoting, tokens);
            return tokens;
        }
        views.push_back(*it);
    }
    return views;
}

std::optional<SharedKey> Normalizer::shared_key(const Spelling& directory,
                                                llvm::StringRef file,
                                                kotatsu::string_list_ref command_line) const {
    auto first = wrapper_prefix_len(command_line);
    if(first >= command_line.size()) {
        return std::nullopt;
    }
    llvm::StringRef driver = command_line[first];
    auto family = with_driver_mode(driver_family(driver), driver, command_line);
    if(family == CompilerFamily::NVCC) {
        return std::nullopt;
    }
    // Only spellings no other option shares are masked: in a cl command
    // `-MT` is the runtime library, not a dependency target.
    std::array separate = {"-o",
                           "-MF",
                           "-MT",
                           "-MQ",
                           "-MJ",
                           "--serialize-diagnostics",
                           "-gen-cdb-fragment-path",
                           "-aux-info",
                           "-index-unit-output-path"};
    std::array joined = {"-frandom-seed=",
                         "-foptimization-record-file=",
                         "-fmodule-mapper=",
                         "-fdeps-file=",
                         "-fdeps-target=",
                         "-fdiagnostics-add-output=",
                         "-fprofile-note=",
                         "-MF",
                         "-MT",
                         "-MQ",
                         "-MJ"};
    // After `/` or `-`; a longer name ahead of one it starts with.
    std::array<llvm::StringRef, 15> cl_outputs = {"Fo",
                                                  "Fd",
                                                  "Fa",
                                                  "Fe",
                                                  "Fi",
                                                  "Fm",
                                                  "FR",
                                                  "Fr",
                                                  "doc",
                                                  "sourceDependencies:directives",
                                                  "sourceDependencies",
                                                  "clang:-MF",
                                                  "clang:-MT",
                                                  "clang:-MQ",
                                                  "clang:-MJ"};
    auto visibility = family_visibility(family);
    std::string text(directory.str());
    // Paths read through a mount table normalize otherwise.
    text += mounts ? '\1' : '\2';
    std::size_t files = 0;
    bool value_next = false;
    for(std::size_t i = 0; i < command_line.size(); i += 1) {
        llvm::StringRef token = command_line[i];
        text += '\0';
        if(token.starts_with("@")) {
            return std::nullopt;
        }
        if(value_next) {
            value_next = false;
            text += "\x01value";
            continue;
        }
        if(token == file) {
            files += 1;
            text += "\x01file";
            continue;
        }
        if(cl_mode(family)) {
            // cl's own outputs, and the dependency files of clang's options
            // passed through `/clang:` (Unreal's `/clang:-MF<file>`); not the
            // program or a path another option takes, which a POSIX host
            // spells with a `/` (`/docker/bin/clang-cl`,
            // `/imsvc /docker/include`).
            auto output = ranges::find_if(cl_outputs, [&](llvm::StringRef name) {
                return (token.starts_with("/") || token.starts_with("-")) &&
                       token.drop_front().starts_with(name);
            });
            if(i <= first || output == cl_outputs.end() ||
               (i > first + 1 && takes_next(command_line[i - 1], token, visibility))) {
                text += token;
                continue;
            }
            text += token.take_front(output->size() + 1);
            // The value apart: `/Fo: <file>`, `/sourceDependencies <file>`,
            // `/clang:-MF /clang:<file>`, which the driver reads together.
            if(i + 1 < command_line.size()) {
                llvm::StringRef next = command_line[i + 1];
                value_next = output->starts_with("clang:")
                                 ? token.size() == output->size() + 1 &&
                                       (next.starts_with("/") || next.starts_with("-")) &&
                                       next.drop_front().starts_with("clang:")
                                 : takes_next(token, next, visibility);
            }
            continue;
        }
        if(ranges::contains(separate, token)) {
            value_next = true;
            text += token;
        } else if(auto prefix = ranges::find_if(
                      joined,
                      [&](llvm::StringRef prefix) { return token.starts_with(prefix); });
                  prefix != joined.end()) {
            text += *prefix;
        } else if((token.starts_with("-Wp,-MD,") || token.starts_with("-Wp,-MMD,")) &&
                  token.count(',') == 2) {
            text += token.take_front(token.rfind(',') + 1);
        } else {
            text += token;
        }
    }
    if(files != 1) {
        return std::nullopt;
    }
    auto hash = llvm::xxh3_128bits(llvm::arrayRefFromStringRef(text));
    return SharedKey{hash.high64, hash.low64};
}

std::vector<std::string> Normalizer::expand_response_files(kotatsu::string_list_ref arguments,
                                                           const Spelling& directory,
                                                           CompilerFamily family,
                                                           unsigned depth) {
    /// The text of the file `name`, which is among the load's inputs, read
    /// or not.
    auto read = [&](kotatsu::buffer_ref out, std::string_view name) -> std::error_code {
        auto file = files.intern(spelled(name, directory, mounts));
        auto observed = vfs::read_observed(files.resolve(file));
        if(observed) {
            files.disk.observe(file, observed->obs);
        }
        if(inputs) {
            inputs->push_back(
                {.file = file,
                 .hash = observed ? std::optional(observed->obs.hash) : std::nullopt});
        }
        if(!observed) {
            return observed.error().code;
        }
        out.assign(observed->text);
        return {};
    };
    auto quoting = cl_mode(family) ? sys::cmd::Quoting::Windows : sys::cmd::Quoting::POSIX;
    /// An unreadable response file stays as written, an input the parse
    /// tells apart (Normalizer::unreadable).
    auto expanded =
        ranges::to<std::vector<std::string>>(sys::cmd::expand(arguments, quoting, read));
    if(family != CompilerFamily::NVCC) {
        return expanded;
    }

    /// nvcc's options files splice in where they stand. A cycle of them
    /// stops 8 files deep.
    std::vector<std::string> spliced;
    for(std::size_t i = 0; i < expanded.size();) {
        auto start = i;
        auto names = options_files(expanded, i);
        if(!names || depth >= 8) {
            spliced.insert(spliced.end(),
                           std::make_move_iterator(expanded.begin() + start),
                           std::make_move_iterator(expanded.begin() + i));
            continue;
        }
        for(auto& name: *names) {
            std::string text;
            if(auto error = read(text, name)) {
                /// nvcc warns about an options file it cannot read and
                /// compiles without it.
                LOG_WARN("Cannot read the nvcc options file {}: {}", name, error.message());
                continue;
            }
            spliced.append_range(
                expand_response_files(split_command(text, quoting), directory, family, depth + 1));
        }
    }
    return spliced;
}

std::optional<CommandID> WrittenCommands::intern(const Spelling& directory,
                                                 kotatsu::string_list_ref arguments) {
    std::string key = directory.str();
    for(auto argument: arguments) {
        key += '\0';
        key += argument;
    }
    auto [it, inserted] = interned.try_emplace(key, invalid_command);
    if(inserted) {
        Normalizer normalizer{.commands = commands, .files = files};
        if(auto normalized = normalizer.normalize(directory, Fid{}, arguments)) {
            it->second = *normalized;
        } else {
            LOG_WARN("Not a compile command: {}", join_command(arguments));
        }
    }
    if(it->second == invalid_command) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace clice
