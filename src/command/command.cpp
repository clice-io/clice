#include "command/command.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <ranges>
#include <string_view>

#include "simdjson.h"
#include "command/nvcc.h"
#include "command/search_config.h"
#include "command/toolchain.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/xxhash.h"
#include "clang/Driver/Types.h"

namespace clice {

namespace {

namespace ranges = std::ranges;

/// Version salt of the persistent command identity: entry hashes change
/// wholesale on schema changes even when old and new renders happen to
/// produce the same bytes.
constexpr llvm::StringRef identity_salt = "clice-cmd-v8";

/// Pre-dedup form of an argument: value storage owned locally until the
/// config wins insertion into the pool.
struct LocalArg {
    std::uint32_t opt_id = 0;
    ArgClass cls = ArgClass::Semantic;
    const char* spelling = nullptr;
    llvm::SmallVector<const char*, 2> values;
};

ArgClass classify(unsigned id, llvm::ArrayRef<const char*> values) {
    /// -fmodule-file has two shapes: `name=path` names a prebuilt module
    /// (clice builds its own PCMs — irrelevant), a bare path is a header
    /// unit that stays part of the frontend semantics.
    if(id == option::OPT_fmodule_file) {
        bool named = values.size() == 1 && llvm::StringRef(values[0]).contains('=');
        return named ? ArgClass::Discarded : ArgClass::Semantic;
    }
    if(is_discarded_option(id)) {
        return ArgClass::Discarded;
    }
    if(is_codegen_option(id)) {
        return ArgClass::Codegen;
    }
    if(is_user_content_option(id)) {
        return ArgClass::UserContent;
    }
    if(is_diagnostics_option(id)) {
        return ArgClass::Diagnostics;
    }
    return ArgClass::Semantic;
}

/// clang's own extension→language mapping (Types.def), the prediction of
/// how the driver classifies a file it is handed bare. Returns the -x
/// language name, or empty when clang has no mapping for the extension.
llvm::StringRef driver_language_for_extension(llvm::StringRef ext) {
    namespace types = clang::driver::types;
    if(ext.empty()) {
        return {};
    }
    auto type = types::lookupTypeForExtension(ext);
    if(type == types::TY_INVALID) {
        return {};
    }
    return types::getTypeName(type);
}

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

bool is_wrapper_name(llvm::StringRef filename) {
    /// Windows tools emit spellings like CCACHE.EXE — match lowercased.
    std::string lowered = filename.lower();
    llvm::StringRef name = lowered;
    name.consume_back(".exe");
    return name == "ccache" || name == "sccache" || name == "distcc" || name == "icecc";
}

/// An appended -gencode adds its architecture next to the base's, the way
/// nvcc itself accumulates them — resolve the arch flags to the newest.
void collapse_gpu_arch_args(std::vector<LocalArg>& args) {
    llvm::SmallVector<std::pair<ArchFlagKind, llvm::StringRef>> sequence;
    llvm::SmallVector<std::size_t> positions;
    for(std::size_t i = 0; i < args.size(); i += 1) {
        auto& arg = args[i];
        std::optional<ArchFlagKind> kind;
        switch(arg.opt_id) {
            case option::OPT_cuda_gpu_arch_EQ: kind = ArchFlagKind::GpuArch; break;
            case option::OPT_offload_arch_EQ: kind = ArchFlagKind::OffloadArch; break;
            case option::OPT_no_offload_arch_EQ: kind = ArchFlagKind::NoOffloadArch; break;
        }
        if(kind && arg.values.size() == 1) {
            sequence.push_back({*kind, arg.values[0]});
            positions.push_back(i);
        }
    }

    auto dropped = collapse_gpu_archs(sequence);
    if(!dropped) {
        return;
    }
    for(auto index: *dropped | std::views::reverse) {
        args.erase(args.begin() + positions[index]);
    }
}

/// KEY=VAL-shaped token — a wrapper option's separate value (`ccache
/// --set-config max_size=1G`), never the compiler: the key admits only
/// [A-Za-z0-9_], which no driver path satisfies up to an '='.
bool is_assignment_token(llvm::StringRef token) {
    auto eq = token.find('=');
    if(eq == llvm::StringRef::npos || eq == 0) {
        return false;
    }
    return llvm::all_of(token.take_front(eq), [](char c) { return llvm::isAlnum(c) || c == '_'; });
}

/// Leading tokens forming a compiler-launcher prefix (ccache, distcc, ...,
/// possibly chained), including the wrapper's own leading options. Zero when
/// the command starts with the compiler itself.
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

std::uint64_t hash_bytes(llvm::StringRef bytes) {
    return llvm::xxh3_64bits(bytes);
}

}  // namespace

void render_arg(const Arg& arg, llvm::function_ref<void(std::string_view)> cb) {
    if(arg.opt_id == option::OPT_UNKNOWN) {
        cb(arg.spelling);
        for(const char* value: arg.values) {
            cb(value);
        }
        return;
    }
    kota::option::ParsedArg parsed;
    parsed.id = arg.opt_id;
    for(const char* value: arg.values) {
        parsed.add_value(value);
    }
    auto forward = [&](std::string_view fragment) {
        cb(fragment);
    };
    option::table().render(parsed, forward);
}

unsigned family_visibility(CompilerFamily family) {
    /// Exclude the slash-prefixed CL and DXC options otherwise (/D and /I
    /// carry both bits), to prevent /U, /D, /I from matching Unix absolute
    /// paths like /Users/... .
    if(family == CompilerFamily::MSVC || family == CompilerFamily::ClangCL) {
        return ~0u;
    }
    return ~static_cast<unsigned>(option::CLOption | option::DXCOption);
}

std::vector<std::string> to_strings(llvm::ArrayRef<const char*> argv) {
    std::vector<std::string> result;
    result.reserve(argv.size());
    for(const char* arg: argv) {
        result.emplace_back(arg);
    }
    return result;
}

CompilationDatabase::CompilationDatabase() : chain(std::make_unique<Toolchain>(*this)) {}

CompilationDatabase::~CompilationDatabase() = default;

void CompilationDatabase::set_workspace_root(llvm::StringRef root) {
    workspace_root = root.str();
}

const CompileConfig& CompilationDatabase::config(ConfigID id) const {
    auto ptr = const_cast<ObjectSet<CompileConfig>&>(configs).get(static_cast<std::uint32_t>(id));
    assert(ptr && "invalid ConfigID");
    return *ptr;
}

llvm::ArrayRef<const char*>
    CompilationDatabase::persist_strings(llvm::ArrayRef<const char*> values) {
    if(values.empty()) {
        return {};
    }
    auto* buf = allocator->Allocate<const char*>(values.size());
    ranges::copy(values, buf);
    return {buf, values.size()};
}

ConfigID CompilationDatabase::save_config(CompileConfig config, llvm::ArrayRef<Arg> local_args) {
    config.args = local_args;
    auto id = configs.get(config);
    auto stored = configs.get(id);
    if(stored->args.data() == local_args.data()) {
        /// Freshly inserted: deep-persist the argument array (values are
        /// interned strings already; their arrays still live in the local
        /// staging storage).
        auto* args = allocator->Allocate<Arg>(local_args.size());
        for(std::size_t i = 0; i < local_args.size(); i += 1) {
            args[i] = local_args[i];
            args[i].values = persist_strings(local_args[i].values);
        }
        stored->args = {args, local_args.size()};
    }
    return ConfigID(id);
}

std::optional<CompilationDatabase::NormalizeResult>
    CompilationDatabase::normalize(llvm::StringRef directory,
                                   std::uint32_t file,
                                   llvm::ArrayRef<const char*> arguments) {
    if(arguments.empty()) {
        return std::nullopt;
    }

    NormalizeResult result;

    /// Wrapper stripping: the prefix is entry provenance, not config
    /// identity — `ccache clang++ X` and `clang++ X` dedupe to one config.
    std::size_t wrapper_len = wrapper_prefix_len(arguments);
    if(wrapper_len > 0) {
        if(wrapper_len >= arguments.size()) {
            LOG_WARN("Compiler launcher without a compiler: {}", print_argv(arguments));
            return std::nullopt;
        }
        llvm::SmallVector<const char*, 4> wrapper;
        for(const char* token: arguments.take_front(wrapper_len)) {
            wrapper.push_back(strings.save(token).data());
        }
        result.wrapper = persist_strings(wrapper);
        arguments = arguments.drop_front(wrapper_len);
    }

    CompileConfig config;
    config.directory = directory.empty() ? "" : strings.save(directory).data();
    config.driver = strings.save(arguments[0]).data();
    arguments = arguments.drop_front();

    config.family = Toolchain::driver_family(config.driver);

    /// zig cc / zig c++: the two tokens together are the driver identity.
    if(config.family == CompilerFamily::Zig && !arguments.empty() &&
       (llvm::StringRef(arguments[0]) == "cc" || llvm::StringRef(arguments[0]) == "c++")) {
        config.subcommand = strings.save(arguments[0]).data();
        arguments = arguments.drop_front();
    }
    /// --driver-mode may also arrive from inside a response file (clang
    /// interprets it post-expansion); the pre-expansion scan only picks the
    /// response tokenization style.
    auto scan_driver_mode = [&](llvm::ArrayRef<const char*> argv) {
        for(llvm::StringRef token: argv) {
            if(token.consume_front("--driver-mode=") && token == "cl") {
                config.family = CompilerFamily::ClangCL;
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
    expand_response_files(tokens, directory, config.family, local_saver);
    scan_driver_mode(tokens);

    /// ccache's --ccache-skip guards the NEXT token from ccache's own
    /// processing; the token itself belongs to the compiler command.
    if(wrapper_len > 0) {
        llvm::erase_if(tokens,
                       [](const char* token) { return llvm::StringRef(token) == "--ccache-skip"; });
    }

    /// nvcc spellings are rewritten into clang's before the table parse —
    /// the table cannot parse them, and unparsed tokens keep no semantics.
    std::vector<std::string> nvcc_translated;
    if(config.family == CompilerFamily::NVCC) {
        llvm::SmallVector<const char*, 32> argv;
        argv.push_back(config.driver);
        argv.append(tokens.begin(), tokens.end());
        nvcc_translated = translate_nvcc_command(argv, directory);
        tokens.clear();
        for(auto& token: llvm::ArrayRef(nvcc_translated).drop_front()) {
            tokens.push_back(token.c_str());
        }
    }

    std::vector<std::string> parse_args(tokens.begin(), tokens.end());
    auto parse_options = kota::option::ParseOptions{.dash_dash_parsing = true,
                                                    .visibility = family_visibility(config.family)};

    /// Two passes: the staging pass keeps every parse result alive, so the
    /// classification pass can decide input pairing (which /Tc-/Tp names
    /// the entry file) before it lays down arguments.
    std::vector<std::expected<kota::option::ParsedArg, kota::option::ParseError>> staged;
    for(auto& parsed: option::table().parse(parse_args, parse_options)) {
        staged.push_back(parsed);
    }

    /// Does this token name the entry's file? Compared through the path
    /// pool (canonical spelling + dot removal), relative tokens resolved
    /// against the entry directory — and as spelled, for entries interned
    /// under a relative spelling (tests, hand-built databases).
    auto matches_entry = [&](llvm::StringRef token) {
        if(file == ~0u || token.empty()) {
            return false;
        }
        llvm::SmallString<256> abs;
        if(path::is_absolute(token)) {
            abs = token;
        } else {
            abs = directory;
            path::append(abs, token);
        }
        path::remove_dots(abs, /*remove_dot_dot=*/true);
        return pool.intern(abs) == file || pool.intern(token) == file;
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
    bool remove_pch = false;

    auto place_slot = [&] {
        args.push_back({.opt_id = option::OPT_INPUT, .cls = ArgClass::Input});
        slot_placed = true;
    };

    for(auto& parsed: staged) {
        if(!parsed.has_value()) {
            /// Unparseable tokens keep their verbatim spelling: dropping an
            /// option we don't understand could merge identities that must
            /// differ. They never reach a compile render.
            auto index = parsed.error().index;
            if(index < parse_args.size()) {
                args.push_back({.opt_id = option::OPT_UNKNOWN,
                                .cls = ArgClass::Unknown,
                                .spelling = strings.save(parse_args[index]).data()});
            }
            continue;
        }

        auto& arg = *parsed;
        auto id = arg.id;

        if(id == option::OPT_UNKNOWN) {
            if(arg.index < parse_args.size()) {
                args.push_back({.opt_id = option::OPT_UNKNOWN,
                                .cls = ArgClass::Unknown,
                                .spelling = strings.save(parse_args[arg.index]).data()});
            }
            continue;
        }

        /// The entry's own input token becomes the slot, preserving its
        /// position (language selectors before it govern it). Other inputs
        /// — nvcc probe leftovers aside, a multi-input command — drop,
        /// paired with their per-file selectors. Input args carry their
        /// token as the spelling.
        ///
        /// NVCC is the exception: the translation already resolved every
        /// positional semantic (nvcc options are command-wide last-wins)
        /// and parks accumulated state after the original input's spot —
        /// the slot goes to the end so edits keep landing after it.
        if(id == option::OPT_INPUT) {
            if(config.family == CompilerFamily::NVCC) {
                continue;
            }
            llvm::StringRef token =
                arg.values.empty() ? llvm::StringRef(arg.spelling) : llvm::StringRef(arg.values[0]);
            if(!slot_placed && matches_entry(token)) {
                place_slot();
            }
            continue;
        }

        if(id == option::OPT__SLASH_Tc || id == option::OPT__SLASH_Tp) {
            if(!slot_placed && arg.values.size() == 1 && matches_entry(arg.values[0])) {
                /// Rewritten to the equivalent global selector: a slot
                /// attribute keyed by the file would break the
                /// (config, rule set) memo — the same config can serve
                /// entries with different per-file selector values.
                args.push_back({.opt_id = *forced_selector, .cls = ArgClass::Semantic});
                place_slot();
            }
            continue;
        }

        if((id == option::OPT__SLASH_TC || id == option::OPT__SLASH_TP) && forced_selector) {
            continue;
        }

        /// CMake's Xclang PCH workaround:
        /// -Xclang -include-pch -Xclang <pchfile> → discard both pairs.
        /// The PCH may be produced by GCC or a different clang version we
        /// cannot load; the plain -include survives and our own preamble
        /// PCH covers it.
        if(is_xclang_option(id) && arg.values.size() == 1) {
            if(remove_pch) {
                remove_pch = false;
                continue;
            }
            if(std::string_view(arg.values[0]) == "-include-pch") {
                remove_pch = true;
                continue;
            }
        }

        LocalArg local;
        local.opt_id = id;
        for(auto value: arg.values) {
            local.values.push_back(strings.save(value).data());
        }
        local.cls = classify(id, local.values);

        /// Include-path values absolutize against the entry directory, so
        /// the config keeps meaning when consumed away from it.
        if(is_include_path_option(id) && local.values.size() == 1) {
            llvm::StringRef value(local.values[0]);
            if(!value.empty() && !path::is_absolute(value)) {
                local.values[0] = strings.save(path::join(directory, value)).data();
            }
        }

        args.push_back(std::move(local));
    }

    if(!slot_placed) {
        place_slot();
    }

    /// Build the pointer-stable Arg view over the staging storage; the
    /// values arrays are deep-persisted only if the config wins insertion.
    llvm::SmallVector<Arg, 32> local_args;
    local_args.reserve(args.size());
    for(auto& local: args) {
        local_args.push_back({.opt_id = local.opt_id,
                              .cls = local.cls,
                              .spelling = local.spelling,
                              .values = local.values});
    }

    result.config = save_config(config, local_args);
    return result;
}

std::optional<CompilationDatabase::NormalizeResult>
    CompilationDatabase::normalize(llvm::StringRef directory,
                                   std::uint32_t file,
                                   llvm::StringRef command) {
    llvm::BumpPtrAllocator local;
    llvm::StringSaver saver(local);

    llvm::SmallVector<const char*, 32> arguments;

#ifdef _WIN32
    llvm::cl::TokenizeWindowsCommandLineFull(command, saver, arguments);
#else
    llvm::cl::TokenizeGNUCommandLine(command, saver, arguments);
#endif

    if(arguments.empty()) {
        return std::nullopt;
    }

    return normalize(directory, file, arguments);
}

void CompilationDatabase::expand_response_files(llvm::SmallVectorImpl<const char*>& tokens,
                                                llvm::StringRef directory,
                                                CompilerFamily family,
                                                llvm::StringSaver& saver,
                                                unsigned depth) {
    /// Depth cap breaks @a → @b → @a cycles.
    if(depth >= 8 || ranges::none_of(tokens, [](const char* token) { return token[0] == '@'; })) {
        return;
    }

    llvm::SmallVector<const char*, 32> expanded;
    for(const char* token: tokens) {
        llvm::StringRef ref(token);
        if(!ref.starts_with("@")) {
            expanded.push_back(token);
            continue;
        }

        llvm::StringRef spec = ref.drop_front();
        std::string full = path::is_absolute(spec) ? spec.str() : path::join(directory, spec);
        auto content = fs::read(full);
        if(!content) {
            /// Unreadable response file: the token survives verbatim (the
            /// real compile would fail the same way).
            expanded.push_back(token);
            continue;
        }

        /// UTF-16 response files (MSVC tooling emits them) convert first.
        llvm::StringRef text(*content);
        std::string utf8;
        if(text.size() >= 2 &&
           ((text[0] == '\xff' && text[1] == '\xfe') || (text[0] == '\xfe' && text[1] == '\xff'))) {
            llvm::ArrayRef<char> bytes(text.data(), text.size());
            if(!llvm::convertUTF16ToUTF8String(bytes, utf8)) {
                LOG_WARN("Cannot decode UTF-16 response file {}", full);
                expanded.push_back(token);
                continue;
            }
            text = utf8;
        }

        llvm::SmallVector<const char*, 32> inner;
        if(family == CompilerFamily::MSVC || family == CompilerFamily::ClangCL) {
            llvm::cl::TokenizeWindowsCommandLineFull(text, saver, inner);
        } else {
            llvm::cl::TokenizeGNUCommandLine(text, saver, inner);
        }
        expand_response_files(inner, directory, family, saver, depth + 1);
        expanded.append(inner.begin(), inner.end());
    }
    tokens = std::move(expanded);
}

void CompilationDatabase::render_identity(ConfigID id, std::string& out) {
    auto& cfg = config(id);
    auto append = [&](std::string_view fragment) {
        out += fragment;
        out += '\0';
    };

    append(cfg.driver);
    if(cfg.subcommand) {
        append(cfg.subcommand);
    }
    for(auto& arg: cfg.args) {
        switch(arg.cls) {
            case ArgClass::Semantic:
            case ArgClass::UserContent:
            case ArgClass::Diagnostics: render_arg(arg, append); break;
            case ArgClass::Unknown: append(arg.spelling); break;
            case ArgClass::Input: append("\x01input"); break;
            case ArgClass::Codegen:
            case ArgClass::Discarded: break;
        }
    }
}

std::uint64_t CompilationDatabase::entry_hash(ConfigID id) {
    auto [it, inserted] = entry_hashes.try_emplace(static_cast<std::uint32_t>(id), 0);
    if(!inserted) {
        return it->second;
    }

    std::string buf;
    buf += identity_salt;
    buf += '\0';
    render_identity(id, buf);
    buf += config(id).directory;
    it->second = hash_bytes(buf);
    return it->second;
}

std::string CompilationDatabase::entry_hash_hex(ConfigID id) {
    return std::format("{:016x}", entry_hash(id));
}

void CompilationDatabase::sort_entries(std::vector<CompilationEntry>& list) {
    /// Hash-equal candidates (codegen-only differences, wrapper-only
    /// differences) still need a stable order: the full render decides,
    /// content-based, so generator reordering never flips the default
    /// selection.
    /// Memoized in a pre-sized vector: the comparator materializes two keys
    /// in one expression, so the memo storage must not relocate mid-compare
    /// (a growing map would).
    std::vector<std::optional<std::string>> full_keys(list.size());
    auto full_key = [&](std::size_t index) -> const std::string& {
        auto& slot = full_keys[index];
        if(!slot) {
            auto& entry = list[index];
            auto& out = slot.emplace();
            auto append = [&](std::string_view fragment) {
                out += fragment;
                out += '\0';
            };
            for(const char* token: entry.wrapper) {
                append(token);
            }
            auto& cfg = config(entry.config);
            append(cfg.driver);
            if(cfg.subcommand) {
                append(cfg.subcommand);
            }
            std::size_t index_of_slot = 0;
            for(auto& arg: cfg.args) {
                if(arg.cls == ArgClass::Input) {
                    break;
                }
                index_of_slot += 1;
            }
            out += std::format("{}", index_of_slot);
            out += '\0';
            for(auto& arg: cfg.args) {
                if(arg.cls == ArgClass::Input) {
                    continue;
                }
                render_arg(arg, append);
            }
        }
        return *slot;
    };

    std::vector<std::size_t> order(list.size());
    for(std::size_t i = 0; i < order.size(); i += 1) {
        order[i] = i;
    }
    ranges::sort(order, [&](std::size_t a, std::size_t b) {
        if(list[a].file != list[b].file) {
            return list[a].file < list[b].file;
        }
        auto ha = entry_hash(list[a].config);
        auto hb = entry_hash(list[b].config);
        if(ha != hb) {
            return ha < hb;
        }
        return full_key(a) < full_key(b);
    });

    std::vector<CompilationEntry> sorted;
    sorted.reserve(list.size());
    for(auto index: order) {
        sorted.push_back(list[index]);
    }
    list = std::move(sorted);
}

std::optional<std::size_t> CompilationDatabase::load(llvm::StringRef path) {
    simdjson::padded_string json_buf;
    if(auto error = simdjson::padded_string::load(std::string(path)).get(json_buf)) {
        LOG_ERROR("Failed to read compilation database from {}: {}",
                  path,
                  simdjson::error_message(error));
        return std::nullopt;
    }

    simdjson::ondemand::parser json_parser;
    simdjson::ondemand::document doc;
    if(auto error = json_parser.iterate(json_buf).get(doc)) {
        LOG_ERROR("Failed to parse compilation database from {}: {}",
                  path,
                  simdjson::error_message(error));
        return std::nullopt;
    }

    simdjson::ondemand::array arr;
    if(auto error = doc.get_array().get(arr)) {
        LOG_ERROR("Invalid compilation database format in {}: root element must be an array.",
                  path);
        return std::nullopt;
    }

    // Parse into a local vector and only swap it in at the end: a file that
    // fails to read or parse at the top level leaves the loaded entries
    // intact, so reload_and_diff() reports no change instead of dropping
    // every file. A file truncated mid-array is NOT caught here (the
    // entries before the cut still swap in) — the CDB poll's two-tick
    // settle debounce is what keeps half-written files from being read.
    std::vector<CompilationEntry> new_entries;

    std::size_t index = 0;
    for(auto element: arr) {
        auto skip = llvm::make_scope_exit([&] { index += 1; });

        simdjson::ondemand::object obj;
        if(element.get_object().get(obj)) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}: "
                "item is not an object.",
                path,
                index);
            continue;
        }

        std::string_view dir_sv, file_sv;
        if(obj["directory"].get_string().get(dir_sv)) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}: "
                "'directory' key is missing.",
                path,
                index);
            continue;
        }

        if(obj["file"].get_string().get(file_sv)) {
            LOG_ERROR(
                "Invalid compilation database in {}. Skipping item at index {}: "
                "'file' key is missing.",
                path,
                index);
            continue;
        }

        llvm::StringRef dir_ref(dir_sv.data(), dir_sv.size());
        llvm::StringRef file_ref(file_sv.data(), file_sv.size());

        // A relative `directory` anchors to the CDB file's own location —
        // self-contained, so every consumer of the same file (server,
        // batch, inspect) resolves it identically.
        llvm::SmallString<256> dir_abs;
        if(!path::is_absolute(dir_ref)) {
            dir_abs = path::parent_path(path);
            fs::make_absolute(dir_abs);
            path::append(dir_abs, dir_ref);
            path::remove_dots(dir_abs, /*remove_dot_dot=*/true);
            dir_ref = dir_abs;
        }

        // Skip non-C-family files (e.g. .rc, .asm, .def) that some build
        // systems emit into compile_commands.json.
        if(!is_c_family_file(file_ref)) {
            continue;
        }

        // Resolve relative file paths against the directory and drop . and
        // .. segments: clang reports realpath'd spellings, and an entry
        // interned with dot segments would never match them.
        llvm::SmallString<256> file_abs;
        if(path::is_absolute(file_ref)) {
            file_abs = file_ref;
        } else {
            file_abs = dir_ref;
            path::append(file_abs, file_ref);
        }
        path::remove_dots(file_abs, /*remove_dot_dot=*/true);
        auto path_id = pool.intern(file_abs);

        std::optional<NormalizeResult> normalized;

        simdjson::ondemand::array args_arr;
        if(!obj["arguments"].get_array().get(args_arr)) {
            llvm::BumpPtrAllocator local;
            llvm::StringSaver saver(local);
            llvm::SmallVector<const char*, 32> args;
            bool malformed = false;
            for(auto arg_val: args_arr) {
                std::string_view sv;
                if(arg_val.get_string().get(sv)) {
                    malformed = true;
                    break;
                }
                args.push_back(saver.save(llvm::StringRef(sv.data(), sv.size())).data());
            }
            if(malformed || args.empty()) {
                continue;
            }
            normalized = normalize(dir_ref, path_id, args);
        } else {
            std::string_view cmd_sv;
            if(obj["command"].get_string().get(cmd_sv)) {
                LOG_ERROR(
                    "Invalid compilation database in {}. Skipping item at index {}: "
                    "neither 'arguments' nor 'command' key is present.",
                    path,
                    index);
                continue;
            }
            normalized = normalize(dir_ref, path_id, llvm::StringRef(cmd_sv.data(), cmd_sv.size()));
        }

        if(!normalized) {
            continue;
        }
        new_entries.push_back({path_id, normalized->config, normalized->wrapper});
    }

    sort_entries(new_entries);
    entry_list = std::move(new_entries);
    return entry_list.size();
}

llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::string, 1>>
    CompilationDatabase::command_hash_snapshot() {
    llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::string, 1>> snapshot;
    for(auto& entry: entry_list) {
        snapshot[entry.file].push_back(entry_hash_hex(entry.config));
    }
    // A file's entries have no inherent order for the diff, so sort each
    // list to make the comparison in reload_and_diff() order-independent.
    for(auto& bucket: snapshot) {
        ranges::sort(bucket.second);
    }
    return snapshot;
}

std::optional<std::string> CompilationDatabase::selected_hash(std::uint32_t path_id) {
    auto candidates = candidate_entries(path_id);
    if(candidates.empty()) {
        return std::nullopt;
    }
    return entry_hash_hex(candidates.front().config);
}

std::optional<CDBDiff> CompilationDatabase::reload_and_diff(llvm::StringRef path) {
    auto before = command_hash_snapshot();
    if(!load(path)) {
        // Unreadable or unparsable (e.g. still locked by the generator):
        // the old entries were kept, and the caller must not treat this as
        // "no change" — it has to retry.
        return std::nullopt;
    }
    auto after = command_hash_snapshot();

    CDBDiff diff;

    for(auto& bucket: after) {
        auto it = before.find(bucket.first);
        if(it == before.end()) {
            diff.added.push_back(bucket.first);
        } else if(it->second != bucket.second) {
            diff.changed.push_back(bucket.first);
        }
    }

    for(auto& bucket: before) {
        if(!after.contains(bucket.first)) {
            diff.removed.push_back(bucket.first);
        }
    }

    ranges::sort(diff.added);
    ranges::sort(diff.removed);
    ranges::sort(diff.changed);

    return diff;
}

llvm::ArrayRef<CompilationEntry>
    CompilationDatabase::candidate_entries(std::uint32_t path_id) const {
    auto [first, last] = ranges::equal_range(entry_list, path_id, {}, &CompilationEntry::file);
    if(first == last) {
        return {};
    }
    return {&*first, static_cast<std::size_t>(last - first)};
}

llvm::ArrayRef<CompilationEntry> CompilationDatabase::candidate_entries(llvm::StringRef file) {
    return candidate_entries(pool.intern(file));
}

bool CompilationDatabase::has_entry(llvm::StringRef file) {
    return !candidate_entries(file).empty();
}

llvm::StringRef CompilationDatabase::forced_language(ConfigID id) const {
    return language_state_at_slot(config(id).args);
}

InputKind CompilationDatabase::input_kind(ConfigID id, llvm::StringRef file) {
    auto state = language_state_at_slot(config(id).args);
    if(!state.empty()) {
        return {strings.save(state).data()};
    }

    auto ext = path::extension(file);
    ext.consume_front(".");
    /// CUDA's header convention is missing from clang's extension table.
    if(ext == "cuh") {
        return {strings.save("cuda").data()};
    }
    if(auto lang = driver_language_for_extension(ext); !lang.empty()) {
        return {strings.save(lang).data()};
    }
    /// No mapping: the raw extension keys the probe (the driver sees a
    /// temp file with the same extension, exactly as confused as it would
    /// be by the real file).
    return {strings.save(ext).data()};
}

ConfigID CompilationDatabase::apply_rules(ConfigID id, const CommandOptions& options) {
    if(options.empty()) {
        return id;
    }

    /// Rule-set identity for the memo: the exact edit content.
    std::string rule_key;
    auto append_section = [&](llvm::ArrayRef<std::string> section) {
        for(auto& item: section) {
            rule_key += item;
            rule_key += '\0';
        }
        rule_key += '\1';
    };
    append_section(options.remove);
    append_section(options.append);
    append_section(options.extra_prepend);
    append_section(options.extra_append);

    auto rule_set_id = rule_set_ids.try_emplace(rule_key, rule_set_ids.size()).first->second;
    auto [memo, inserted] =
        rule_applied.try_emplace({static_cast<std::uint32_t>(id), rule_set_id}, 0);
    if(!inserted) {
        return ConfigID(memo->second);
    }

    const auto& cfg = config(id);
    bool is_nvcc = cfg.family == CompilerFamily::NVCC;
    llvm::StringRef directory = cfg.directory;

    /// Rule flags for an NVCC entry arrive in the same nvcc spellings as
    /// the command they edit — rewrite them like the command itself.
    /// Appends translate as one command edit, so an appended default state
    /// (`-rdc=false` over an rdc base) cancels the base's translated state
    /// instead of vanishing.
    auto translate_rule_flags = [&](llvm::ArrayRef<std::string> rule_flags, bool edit) {
        std::vector<std::string> flags(rule_flags.begin(), rule_flags.end());
        if(flags.empty() || !is_nvcc) {
            return flags;
        }
        std::vector<const char*> argv;
        argv.reserve(flags.size() + 1);
        argv.push_back(cfg.driver);
        for(auto& flag: flags) {
            argv.push_back(flag.c_str());
        }
        auto translated = translate_nvcc_command(argv, directory, edit);
        flags.assign(std::make_move_iterator(translated.begin() + 1),
                     std::make_move_iterator(translated.end()));
        return flags;
    };

    std::vector<std::string> remove_source(options.remove.begin(), options.remove.end());
    if(is_nvcc) {
        /// A wildcard arch removal (`-arch=*`, `--generate-code=*`) must
        /// clear whichever form the translated base carries: numeric archs
        /// become `--cuda-gpu-arch=`, non-numeric selections persist as
        /// `-arch=` probe tokens — rewrite to both wildcards.
        for(std::size_t i = 0; i < remove_source.size(); i += 1) {
            llvm::StringRef flag = remove_source[i];
            for(llvm::StringRef spelling:
                {"-arch", "--gpu-architecture", "-gencode", "--generate-code"}) {
                bool joined = flag.starts_with(spelling) && flag.substr(spelling.size()) == "=*";
                bool separate =
                    flag == spelling && i + 1 < remove_source.size() && remove_source[i + 1] == "*";
                if(!joined && !separate) {
                    continue;
                }
                if(separate) {
                    remove_source.erase(remove_source.begin() + i + 1);
                }
                remove_source[i] = "--cuda-gpu-arch=*";
                remove_source.insert(remove_source.begin() + i + 1, "-arch=*");
                i += 1;
                break;
            }
        }
    }

    /// Remove patterns are an independent list, not one command: translated
    /// whole, nvcc's last-wins would swallow every alternative value of a
    /// stateful option but the last. Each pattern translates alone —
    /// standalone, so it reproduces exactly the flags the base translation
    /// emitted — pairing a separate value token (never dash-led) with its
    /// spelling.
    std::vector<std::string> remove_flags;
    if(is_nvcc) {
        for(std::size_t i = 0; i < remove_source.size(); i += 1) {
            std::size_t count = 1;
            if(llvm::StringRef(remove_source[i]).starts_with("-") && i + 1 < remove_source.size() &&
               !llvm::StringRef(remove_source[i + 1]).starts_with("-")) {
                count = 2;
            }
            auto pattern = translate_rule_flags(llvm::ArrayRef(remove_source).slice(i, count),
                                                /*edit=*/false);
            remove_flags.insert(remove_flags.end(),
                                std::make_move_iterator(pattern.begin()),
                                std::make_move_iterator(pattern.end()));
            i += count - 1;
        }
    } else {
        remove_flags = std::move(remove_source);
    }

    std::vector<kota::option::ParsedArg> remove_args;
    auto remove_parse_options =
        kota::option::ParseOptions{.visibility = family_visibility(cfg.family)};
    for(auto& parsed: option::table().parse(remove_flags, remove_parse_options)) {
        if(parsed.has_value()) {
            remove_args.push_back(*parsed);
        }
    }
    auto get_id = [](const kota::option::ParsedArg& arg) {
        return arg.id;
    };
    ranges::sort(remove_args, {}, get_id);

    auto matches_remove = [&](const Arg& arg) {
        auto range = ranges::equal_range(remove_args, arg.opt_id, {}, get_id);
        for(auto& remove: range) {
            /// All unknown options share one id; their identity is the
            /// spelling (NVCC probe flags persist as unknown tokens). A
            /// trailing `=*` wildcards the value part, mirroring the
            /// known-option value wildcard below.
            if(arg.opt_id == option::OPT_UNKNOWN) {
                llvm::StringRef pattern = remove.spelling;
                bool wildcard = pattern.consume_back("*") && pattern.ends_with("=");
                if(wildcard ? llvm::StringRef(arg.spelling).starts_with(pattern)
                            : arg.spelling == llvm::StringRef(remove.spelling)) {
                    return true;
                }
                continue;
            }
            if(remove.values.size() == 1 && remove.values[0] == "*") {
                return true;
            }
            if(ranges::equal(arg.values, remove.values, [](const char* a, std::string_view b) {
                   return std::string_view(a) == b;
               })) {
                return true;
            }
        }
        return false;
    };

    /// Parse an edit list into structured args, absolutizing include paths
    /// against the config's directory like the load pipeline. Unknown tokens
    /// keep the user's spelling and stay renderable (the user asked for them
    /// explicitly) — including input-classified ones: an edit cannot name
    /// the entry's input, so such a token is really the separate value of an
    /// option the table does not know.
    auto parse_edit = [&](llvm::ArrayRef<std::string> edit_flags, std::vector<LocalArg>& out) {
        std::vector<std::string> flags(edit_flags.begin(), edit_flags.end());
        for(auto& parsed: option::table().parse(flags, remove_parse_options)) {
            if(!parsed.has_value()) {
                auto index = parsed.error().index;
                if(index < flags.size()) {
                    out.push_back({.opt_id = option::OPT_UNKNOWN,
                                   .cls = ArgClass::Semantic,
                                   .spelling = strings.save(flags[index]).data()});
                }
                continue;
            }
            auto& arg = *parsed;
            if(arg.id == option::OPT_INPUT || arg.id == option::OPT_UNKNOWN) {
                if(arg.index < flags.size()) {
                    out.push_back({.opt_id = option::OPT_UNKNOWN,
                                   .cls = ArgClass::Semantic,
                                   .spelling = strings.save(flags[arg.index]).data()});
                }
                continue;
            }
            LocalArg local;
            local.opt_id = arg.id;
            for(auto value: arg.values) {
                local.values.push_back(strings.save(value).data());
            }
            local.cls = classify(arg.id, local.values);
            if(is_include_path_option(arg.id) && local.values.size() == 1) {
                llvm::StringRef value(local.values[0]);
                if(!value.empty() && !path::is_absolute(value)) {
                    local.values[0] = strings.save(path::join(directory, value)).data();
                }
            }
            out.push_back(std::move(local));
        }
    };

    std::vector<LocalArg> prepend_args;
    parse_edit(options.extra_prepend, prepend_args);

    std::vector<LocalArg> append_args;
    parse_edit(translate_rule_flags(options.append, /*edit=*/true), append_args);
    parse_edit(options.extra_append, append_args);

    /// Rebuild the sequence: prepends first, base args with removes
    /// cancelled, appends inserted before the input slot — an append always
    /// takes effect for the compile, and with the common input-at-end CDB
    /// the byte order matches the old tail-append exactly.
    std::vector<LocalArg> edited;
    edited.reserve(prepend_args.size() + cfg.args.size() + append_args.size());
    for(auto& local: prepend_args) {
        edited.push_back(local);
    }
    for(auto& arg: cfg.args) {
        if(arg.cls == ArgClass::Input) {
            for(auto& local: append_args) {
                edited.push_back(local);
            }
            edited.push_back({.opt_id = option::OPT_INPUT, .cls = ArgClass::Input});
            continue;
        }
        if(matches_remove(arg)) {
            continue;
        }
        LocalArg local;
        local.opt_id = arg.opt_id;
        local.cls = arg.cls;
        local.spelling = arg.spelling;
        local.values.assign(arg.values.begin(), arg.values.end());
        edited.push_back(std::move(local));
    }

    /// An appended -gencode adds its architecture next to the base's, the
    /// way nvcc itself accumulates them — resolve them to the newest.
    if(is_nvcc) {
        collapse_gpu_arch_args(edited);
    }

    llvm::SmallVector<Arg, 32> local_args;
    local_args.reserve(edited.size());
    for(auto& local: edited) {
        local_args.push_back({.opt_id = local.opt_id,
                              .cls = local.cls,
                              .spelling = local.spelling,
                              .values = local.values});
    }

    CompileConfig result = cfg;
    auto result_id = save_config(result, local_args);
    rule_applied[{static_cast<std::uint32_t>(id), rule_set_id}] =
        static_cast<std::uint32_t>(result_id);
    return result_id;
}

ConfigID CompilationDatabase::fallback_config(llvm::StringRef file) {
    // Synthesize a default command so the file still compiles and produces
    // diagnostics instead of failing silently. Config rule appends apply on
    // top through the regular apply_rules path: users without a CDB rely on
    // them to supply include paths.
    llvm::SmallVector<const char*, 8> arguments;
    llvm::StringRef variant;
    if(file.ends_with(".cpp") || file.ends_with(".hpp") || file.ends_with(".cc")) {
        variant = "c++";
        arguments = {"clang++", "-std=c++20"};
    } else if(file.ends_with(".cu") || file.ends_with(".cuh")) {
        /// Device-only pins the same device-side view NVCC-backed commands
        /// default to, instead of whichever job the toolchain query happens
        /// to pick from a two-sided compilation; a config rule appending
        /// --cuda-host-only still wins as the later flag.
        variant = "cuda";
        arguments = {"clang++", "-std=c++20", "-x", "cuda", "--cuda-device-only"};
    } else {
        variant = "c";
        arguments = {"clang"};
    }

    auto [it, inserted] = fallback_configs.try_emplace(variant, invalid_config);
    if(inserted) {
        auto normalized = normalize("", ~0u, arguments);
        assert(normalized && "fallback synthesis cannot fail");
        it->second = normalized->config;
    }
    return it->second;
}

std::vector<const char*> CompilationDatabase::render_driver(const CommandRef& ref,
                                                            const RenderOptions& opts) {
    auto& cfg = config(ref.config);
    auto source = pool.resolve(ref.file);

    std::vector<const char*> argv;
    argv.reserve(cfg.args.size() + 8);
    argv.push_back(cfg.driver);
    if(cfg.subcommand) {
        argv.push_back(cfg.subcommand);
    }

    // Inject our resource dir if the command names none, so the embedded
    // frontend and its builtin headers stay version-matched.
    bool has_resource_dir = ranges::any_of(cfg.args, [](const Arg& arg) {
        return arg.opt_id == option::OPT_resource_dir || arg.opt_id == option::OPT_resource_dir_EQ;
    });
    if(!has_resource_dir && !resource_dir().empty()) {
        argv.push_back("-resource-dir");
        argv.push_back(resource_dir().data());
    }

    auto emit = [&](std::string_view fragment) {
        argv.push_back(strings.save(fragment).data());
    };

    auto state = language_state_at_slot(cfg.args);
    std::size_t last_user_content = argv.size();

    for(auto& arg: cfg.args) {
        switch(arg.cls) {
            case ArgClass::Semantic:
            case ArgClass::UserContent:
            case ArgClass::Diagnostics:
                render_arg(arg, emit);
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
                    auto ext = path::extension(source);
                    ext.consume_front(".");
                    auto driver_lang = driver_language_for_extension(ext);
                    if(driver_lang != llvm::StringRef(ref.input.value)) {
                        if(cfg.family == CompilerFamily::MSVC ||
                           cfg.family == CompilerFamily::ClangCL) {
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

std::vector<const char*> CompilationDatabase::render(const CommandRef& ref,
                                                     const RenderOptions& opts) {
    auto resolved = chain->resolve(ref.config, ref.input);
    if(!resolved) {
        LOG_WARN("Toolchain resolve failed for {}: {}", pool.resolve(ref.file), resolved.error());
        return render_driver(ref, opts);
    }

    auto& rc = chain->resolved(*resolved);
    auto source = pool.resolve(ref.file);

    std::vector<const char*> argv;
    argv.reserve(rc.args.size() + 8);
    argv.push_back(rc.driver);
    if(rc.is_cc1) {
        argv.push_back("-cc1");
        argv.push_back("-main-file-name");
        // path::filename returns a suffix of the interned path (a pointer
        // into the same buffer), so .data() is null-terminated.
        argv.push_back(path::filename(source).data());
    }

    auto emit = [&](std::string_view fragment) {
        argv.push_back(strings.save(fragment).data());
    };

    std::size_t last_user_content = argv.size();
    for(auto& arg: rc.args) {
        render_arg(arg, emit);
        if(arg.cls == ArgClass::UserContent) {
            last_user_content = argv.size();
        }
    }

    if(opts.preamble) {
        argv.insert(argv.begin() + last_user_content, {"-include", opts.preamble});
    }

    argv.push_back(source.data());
    return argv;
}

std::vector<const char*> CompilationDatabase::render_full(ConfigID id) {
    auto& cfg = config(id);
    std::vector<const char*> argv;
    argv.reserve(cfg.args.size() + 2);
    argv.push_back(cfg.driver);
    if(cfg.subcommand) {
        argv.push_back(cfg.subcommand);
    }
    auto emit = [&](std::string_view fragment) {
        argv.push_back(strings.save(fragment).data());
    };
    for(auto& arg: cfg.args) {
        if(arg.cls == ArgClass::Input) {
            continue;
        }
        render_arg(arg, emit);
    }
    return argv;
}

void CompilationDatabase::warm(llvm::ArrayRef<CommandRef> refs) {
    llvm::SmallVector<std::pair<ConfigID, InputKind>> pairs;
    pairs.reserve(refs.size());
    for(auto& ref: refs) {
        pairs.push_back({ref.config, ref.input});
    }
    chain->warm(pairs);
}

SearchConfig CompilationDatabase::search_config(const CommandRef& ref) {
    llvm::StringRef directory = config(ref.config).directory;
    auto resolved = chain->resolve(ref.config, ref.input);
    if(!resolved) {
        return extract_search_config(config(ref.config).args, directory);
    }
    auto [it, inserted] = search_configs.try_emplace(*resolved);
    if(inserted) {
        it->second = extract_search_config(chain->resolved(*resolved).args, directory);
    }
    return it->second;
}

#ifdef CLICE_ENABLE_TEST

std::optional<CompilationEntry>
    CompilationDatabase::add_command(llvm::StringRef directory,
                                     llvm::StringRef file,
                                     llvm::ArrayRef<const char*> arguments) {
    auto path_id = pool.intern(file);
    auto normalized = normalize(directory, path_id, arguments);
    if(!normalized) {
        return std::nullopt;
    }
    CompilationEntry entry{path_id, normalized->config, normalized->wrapper};
    entry_list.push_back(entry);
    sort_entries(entry_list);
    return entry;
}

std::optional<CompilationEntry> CompilationDatabase::add_command(llvm::StringRef directory,
                                                                 llvm::StringRef file,
                                                                 llvm::StringRef command) {
    auto path_id = pool.intern(file);
    auto normalized = normalize(directory, path_id, command);
    if(!normalized) {
        return std::nullopt;
    }
    CompilationEntry entry{path_id, normalized->config, normalized->wrapper};
    entry_list.push_back(entry);
    sort_entries(entry_list);
    return entry;
}

#endif

}  // namespace clice
