module clice;

import :command.argument;
import :command.normalize;
import :command.toolchains.gcc;
import :command.toolchains.nvcc;
import :command.table;
import :support.logging;
import :support.shell;
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
        LOG_WARN("Compiler launcher without a compiler: {}", print_argv(command_line));
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
    auto scan_driver_mode = [&](const auto& argv) {
        for(llvm::StringRef token: argv) {
            if(token.consume_front("--driver-mode=")) {
                command.family =
                    token == "cl" ? CompilerFamily::ClangCL : driver_family(command.driver);
            }
        }
    };
    scan_driver_mode(arguments);

    auto parse_args = expand_response_files(arguments, directory, command.family);
    scan_driver_mode(parse_args);

    /// ccache's --ccache-skip guards the NEXT token from ccache's own
    /// processing; the token itself belongs to the compiler command.
    if(wrapper_len > 0) {
        std::erase(parse_args, "--ccache-skip");
    }

    /// nvcc spellings are rewritten into clang's before the table parse —
    /// the table cannot parse them, and unparsed tokens keep no semantics.
    if(command.family == CompilerFamily::NVCC) {
        llvm::SmallVector<const char*, 32> argv = {command.driver};
        for(auto& argument: parse_args) {
            argv.push_back(argument.c_str());
        }
        parse_args = translate_nvcc_command(argv, directory);
        parse_args.erase(parse_args.begin());
    }

    auto parse_options = cli::ParseOptions{.dash_dash_parsing = true,
                                           .visibility = family_visibility(command.family)};

    /// Two passes: the staging pass keeps every parse result alive, so the
    /// classification pass can decide input pairing (which /Tc-/Tp names
    /// the entry file) before it lays down arguments.
    std::vector<std::expected<cli::ParsedArg, cli::Error>> staged;
    for(auto& parsed: clang_options::table().parse(parse_args, parse_options)) {
        staged.push_back(parsed);
    }

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
    bool included_pch = false;

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

        /// CMake passes a target's precompiled header to cc1 directly:
        /// `-Xclang -include-pch -Xclang <pch>` in its units' commands,
        /// `-Xclang -emit-pch` in the one building it. Both take the
        /// driver's spelling, an argument clice manages itself: the file
        /// on disk may come from GCC or another clang, so a unit loads
        /// clice's own build of it instead (Project::pch_writer).
        if(is_xclang_option(id) && parsed->values.size() == 1) {
            llvm::StringRef value = parsed->values[0];
            if(included_pch) {
                included_pch = false;
                args.push_back({
                    .opt_id = clang_options::OPT_include_pch,
                    .cls = ArgClass::Precompiled,
                    .values =
                        {anchored(commands, clang_options::OPT_include_pch, value, anchor, mounts)
                             .data()},
                });
                continue;
            }
            if(value == "-include-pch") {
                included_pch = true;
                continue;
            }
            if(value == "-emit-pch") {
                args.push_back(
                    {.opt_id = clang_options::OPT_emit_pch, .cls = ArgClass::Precompiled});
                continue;
            }
            /// Bazel's module rules (xclang's) embed a module's sources in
            /// it: the flag goes as its driver spelling does
            /// (is_discarded_option).
            if(value == "-fmodules-embed-all-files") {
                continue;
            }
        }

        auto local = commands.local_arg(parsed, parse_args);
        if(!local) {
            continue;
        }

        /// g++ knows warnings clang does not (-Wlogical-op, -Wuseless-cast),
        /// each of which clang would report as unknown on every file.
        if(command.family == CompilerFamily::GCC && id == clang_options::OPT_W_Joined &&
           !clang_warning(local->values[0])) {
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
    // A database written inside Cygwin or MSYS2 quotes its commands for sh.
    return normalize(directory,
                     file,
                     split_command(command, mounts ? sys::cmd::Quoting::POSIX : host_quoting));
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
    for(std::size_t i = 0; i < expanded.size(); i += 1) {
        auto names = depth < 8 ? options_files(expanded, i) : std::nullopt;
        if(!names) {
            spliced.push_back(std::move(expanded[i]));
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
            LOG_WARN("Not a compile command: {}", print_argv(arguments));
        }
    }
    if(it->second == invalid_command) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace clice
