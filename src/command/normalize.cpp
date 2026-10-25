module;

#include "support/prelude.h"

#include "support/logging.macros.h"

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
       (id == option::OPT_include || id == option::OPT_imacros)) {
        std::string windows;
        mounts->to_windows(windows, value);
        return commands.intern(windows);
    }
    if(!names_path(id, value)) {
        return value;
    }
    return commands.intern(spelled(value, anchor, mounts).str());
}

std::size_t wrapper_prefix_len(llvm::ArrayRef<const char*> argv) {
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
                                               llvm::ArrayRef<const char*> arguments) {
    if(arguments.empty()) {
        return std::nullopt;
    }

    /// Wrapper stripping: the prefix is entry provenance, not command
    /// identity — `ccache clang++ X` and `clang++ X` dedupe to one command.
    std::size_t wrapper_len = wrapper_prefix_len(arguments);
    if(wrapper_len > 0) {
        if(wrapper_len >= arguments.size()) {
            LOG_WARN("Compiler launcher without a compiler: {}", print_argv(arguments));
            return std::nullopt;
        }
        arguments = arguments.drop_front(wrapper_len);
    }

    Command command;
    command.directory = commands.intern(directory.str()).data();
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
       (llvm::StringRef(arguments[0]) == "cc" || llvm::StringRef(arguments[0]) == "c++")) {
        command.subcommand = commands.intern(arguments[0]).data();
        arguments = arguments.drop_front();
    }
    /// --driver-mode may also arrive from inside a response file (clang
    /// interprets it post-expansion); the pre-expansion scan only picks the
    /// response tokenization style.
    auto scan_driver_mode = [&](llvm::ArrayRef<const char*> argv) {
        for(llvm::StringRef token: argv) {
            if(token.consume_front("--driver-mode=")) {
                command.family =
                    token == "cl" ? CompilerFamily::ClangCL : driver_family(command.driver);
            }
        }
    };
    scan_driver_mode(arguments);

    /// Response-file expansion, driver-mode aware: CL commands tokenize
    /// with Windows rules regardless of the server platform. Relative
    /// @paths resolve against the entry directory; contents may nest.
    llvm::BumpPtrAllocator local_alloc;
    llvm::StringSaver local_saver(local_alloc);
    llvm::SmallVector<const char*, 32> tokens(arguments.begin(), arguments.end());
    expand_response_files(tokens, directory, command.family, local_saver);
    scan_driver_mode(tokens);

    /// ccache's --ccache-skip guards the NEXT token from ccache's own
    /// processing; the token itself belongs to the compiler command.
    if(wrapper_len > 0) {
        tokens.erase(ranges::remove_if(tokens,
                                       [](const char* token) {
                                           return llvm::StringRef(token) == "--ccache-skip";
                                       })
                         .begin(),
                     tokens.end());
    }

    /// nvcc spellings are rewritten into clang's before the table parse —
    /// the table cannot parse them, and unparsed tokens keep no semantics.
    std::vector<std::string> nvcc_translated;
    if(command.family == CompilerFamily::NVCC) {
        llvm::SmallVector<const char*, 32> argv;
        argv.push_back(command.driver);
        argv.append(tokens.begin(), tokens.end());
        nvcc_translated = translate_nvcc_command(argv, directory);
        tokens.clear();
        for(auto& token: llvm::ArrayRef(nvcc_translated).drop_front()) {
            tokens.push_back(token.c_str());
        }
    }

    std::vector<std::string> parse_args(tokens.begin(), tokens.end());
    auto parse_options =
        kota::option::ParseOptions{.dash_dash_parsing = true,
                                   .visibility = family_visibility(command.family)};

    /// Two passes: the staging pass keeps every parse result alive, so the
    /// classification pass can decide input pairing (which /Tc-/Tp names
    /// the entry file) before it lays down arguments.
    std::vector<std::expected<kota::option::ParsedArg, kota::option::ParseError>> staged;
    for(auto& parsed: option::table().parse(parse_args, parse_options)) {
        staged.push_back(parsed);
    }

    /// Relative paths of the command resolve where the compile runs: the
    /// entry directory, moved by `-working-directory`.
    auto anchor = directory;
    for(auto& parsed: staged) {
        if(parsed && parsed->id == option::OPT_working_directory && parsed->values.size() == 1) {
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
        if((id == option::OPT__SLASH_Tc || id == option::OPT__SLASH_Tp) &&
           parsed->values.size() == 1 && matches_entry(parsed->values[0])) {
            forced_selector =
                id == option::OPT__SLASH_Tc ? option::OPT__SLASH_TC : option::OPT__SLASH_TP;
        }
    }

    std::vector<LocalArg> args;
    args.reserve(staged.size() + 1);
    bool slot_placed = false;
    bool included_pch = false;

    auto place_slot = [&] {
        args.push_back({.opt_id = option::OPT_INPUT, .cls = ArgClass::Input});
        slot_placed = true;
    };

    for(auto& parsed: staged) {
        auto id = parsed ? parsed->id : option::OPT_UNKNOWN;

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
        if(id == option::OPT_INPUT) {
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

        if(id == option::OPT__SLASH_Tc || id == option::OPT__SLASH_Tp) {
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

        if((id == option::OPT__SLASH_TC || id == option::OPT__SLASH_TP) && forced_selector) {
            continue;
        }

        /// CMake passes a target's precompiled header to cc1 directly:
        /// `-Xclang -include-pch -Xclang <pch>` in its units' commands,
        /// `-Xclang -emit-pch` in the one building it. Both take the
        /// driver's spelling, an argument clice manages itself: the file
        /// on disk may come from GCC or another clang, so a unit loads
        /// clice's own build of it instead (Build::pch_unit).
        if(is_xclang_option(id) && parsed->values.size() == 1) {
            llvm::StringRef value = parsed->values[0];
            if(included_pch) {
                included_pch = false;
                args.push_back({
                    .opt_id = option::OPT_include_pch,
                    .cls = ArgClass::Discarded,
                    .values =
                        {anchored(commands, option::OPT_include_pch, value, anchor, mounts).data()},
                });
                continue;
            }
            if(value == "-include-pch") {
                included_pch = true;
                continue;
            }
            if(value == "-emit-pch") {
                args.push_back({.opt_id = option::OPT_emit_pch, .cls = ArgClass::Discarded});
                continue;
            }
        }

        auto local = commands.local_arg(parsed, parse_args);

        /// g++ knows warnings clang does not (-Wlogical-op, -Wuseless-cast),
        /// each of which clang would report as unknown on every file.
        if(command.family == CompilerFamily::GCC && id == option::OPT_W_Joined &&
           !clang_warning(local.values[0])) {
            local.cls = ArgClass::Discarded;
        }

        /// Path values absolutize where the compile runs, so the command
        /// keeps meaning when consumed away from it: the toolchain query
        /// runs elsewhere.
        // The working directory itself is relative to the entry's.
        bool working = id == option::OPT_working_directory;
        for(auto& value: local.values) {
            value = anchored(commands, id, value, working ? directory : anchor, mounts).data();
        }

        args.push_back(std::move(local));
    }

    if(!slot_placed) {
        place_slot();
    }
    return commands.save(command, args);
}

std::optional<CommandID> Normalizer::normalize(const Spelling& directory,
                                               Fid file,
                                               llvm::StringRef command) {
    llvm::BumpPtrAllocator local;
    llvm::StringSaver saver(local);
    // A database written inside Cygwin or MSYS2 quotes its commands for sh.
    return normalize(directory,
                     file,
                     mounts ? tokenize_sh_command(command, saver)
                            : tokenize_command(command, saver));
}

void Normalizer::expand_response_files(llvm::SmallVectorImpl<const char*>& tokens,
                                       const Spelling& directory,
                                       CompilerFamily family,
                                       llvm::StringSaver& saver,
                                       unsigned depth) {
    /// Depth cap breaks @a → @b → @a cycles.
    if(depth >= 8) {
        return;
    }

    llvm::SmallVector<const char*, 32> expanded;

    /// Splice the arguments of the file `name`, false when it cannot be
    /// read. Read or not, it is among the load's inputs.
    auto splice = [&](llvm::StringRef name) {
        auto full = spelled(name, directory, mounts);
        auto file = files.intern(full);
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
            return false;
        }

        /// UTF-16 response files (MSVC tooling emits them) convert first.
        llvm::StringRef text = observed->content->getBuffer();
        std::string utf8;
        if(text.size() >= 2 &&
           ((text[0] == '\xff' && text[1] == '\xfe') || (text[0] == '\xfe' && text[1] == '\xff'))) {
            llvm::ArrayRef<char> bytes(text.data(), text.size());
            if(!llvm::convertUTF16ToUTF8String(bytes, utf8)) {
                LOG_WARN("Cannot decode UTF-16 response file {}", full);
                return false;
            }
            text = utf8;
        }

        llvm::SmallVector<const char*, 32> inner;
        if(cl_mode(family)) {
            llvm::cl::TokenizeWindowsCommandLineFull(text, saver, inner);
        } else {
            llvm::cl::TokenizeGNUCommandLine(text, saver, inner);
        }
        expand_response_files(inner, directory, family, saver, depth + 1);
        expanded.append(inner.begin(), inner.end());
        return true;
    };

    for(std::size_t i = 0; i < tokens.size(); i += 1) {
        llvm::StringRef token(tokens[i]);
        if(token.consume_front("@")) {
            /// Unreadable response file: the token survives verbatim (the
            /// real compile would fail the same way).
            if(!splice(token)) {
                expanded.push_back(tokens[i]);
            }
            continue;
        }
        if(family == CompilerFamily::NVCC) {
            if(auto names = options_files(tokens, i)) {
                /// nvcc warns about an options file it cannot read and
                /// compiles without it.
                for(auto& name: *names) {
                    if(!splice(name)) {
                        LOG_WARN("Cannot read nvcc options file {}", name);
                    }
                }
                continue;
            }
        }
        expanded.push_back(tokens[i]);
    }
    tokens = std::move(expanded);
}

}  // namespace clice
