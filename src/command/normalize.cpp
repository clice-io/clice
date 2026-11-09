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

using ParsedArgs = std::vector<std::expected<cli::ParsedArg, cli::Error>>;

/// `staged` with what its `-Xclang` options pass on read by cc1's options,
/// which clang's driver passes their values to together, in order. The
/// command keeps them as written but for an option clice manages itself or
/// the compile never takes, which goes as its driver spelling does
/// (classify_option). CMake passes a target's precompiled header to cc1
/// directly, `-Xclang -include-pch -Xclang <pch>` in its units' commands,
/// `-Xclang -emit-pch` in the one building it: the file on disk may come
/// from GCC or another clang, so a unit loads clice's own build of it
/// instead (Project::pch_writer). Bazel's module rules (xclang's) embed a
/// module's sources in it (`-fmodules-embed-all-files`).
ParsedArgs read_xclang(ParsedArgs staged, CommandTable& commands) {
    llvm::SmallVector<std::size_t> xclang;
    llvm::SmallVector<std::string_view> passed;
    for(std::size_t i = 0; i < staged.size(); i += 1) {
        if(staged[i] && is_xclang_option(staged[i]->id)) {
            xclang.push_back(i);
            passed.push_back(staged[i]->values[0]);
        }
    }
    // The option an argument reads as in place of the -Xclang options it
    // starts, which go.
    std::vector<std::optional<cli::ParsedArg>> read(staged.size());
    std::vector<bool> gone(staged.size());
    auto cc1 = cli::ParseOptions{.visibility = clang_options::CC1Option};
    for(auto& option: clang_options::table().parse(passed, cc1)) {
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
            gone[xclang[k]] = true;
        }
        read[xclang[option->index]] = std::move(*option);
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
    auto staged = read_xclang(parse_arguments(parse_args, family, parse_options), commands);

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
            if(command.family == CompilerFamily::NVCC) {
                continue;
            }
            llvm::StringRef token = parsed->values.empty() ? llvm::StringRef(parsed->spelling)
                                                           : llvm::StringRef(parsed->values[0]);
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
    return normalize(directory, file, split(command, views, tokens));
}

kotatsu::string_list_ref Normalizer::split(llvm::StringRef command,
                                           llvm::SmallVectorImpl<std::string_view>& views,
                                           std::vector<std::string>& tokens) const {
    // A database written inside Cygwin or MSYS2 quotes its commands for sh.
    auto quoting = mounts ? sys::cmd::Quoting::POSIX : sys::cmd::native_quoting;
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
                         "-fprofile-note="};
    std::string text(directory.str());
    // Paths read through a mount table normalize otherwise.
    text += mounts ? '\1' : '\2';
    std::size_t files = 0;
    bool value_next = false;
    for(llvm::StringRef token: command_line) {
        text += '\0';
        if(token.starts_with("@")) {
            return std::nullopt;
        }
        if(value_next) {
            value_next = false;
            text += "\x01value";
        } else if(token == file) {
            files += 1;
            text += "\x01file";
        } else if(cl_mode(family)) {
            text +=
                token.starts_with("/Fo") || token.starts_with("-Fo") ? token.take_front(3) : token;
        } else if(ranges::contains(separate, token)) {
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
            return observed.error();
        }
        out.assign(observed->text);
        return {};
    };
    auto quoting = cl_mode(family) ? sys::cmd::Quoting::Windows : sys::cmd::Quoting::POSIX;
    /// An unreadable response file stays as written: the real compile fails
    /// the same way.
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
            if(read(text, name)) {
                /// nvcc warns about an options file it cannot read and
                /// compiles without it.
                LOG_WARN("Cannot read nvcc options file {}", name);
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
