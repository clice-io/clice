#include "command/nvcc.h"

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>

#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/StringSaver.h"

namespace clice {

namespace {

constexpr llvm::StringLiteral ccbin_prefix = "-ccbin=";
constexpr llvm::StringLiteral target_directory_prefix = "--target-directory=";
constexpr llvm::StringLiteral allow_unsupported_flag = "--allow-unsupported-compiler";
constexpr llvm::StringLiteral gpu_arch_prefix = "-arch=";

/// One GPU architecture named inside an -arch/-gencode value.
struct ArchToken {
    unsigned number = 0;

    /// 'a' (arch-specific) or 'f' (family) variant, 0 for the plain form.
    char suffix = 0;

    bool operator==(const ArchToken&) const = default;
};

/// Scan a spec like "compute_90a" or "arch=compute_90a" for sm_NN /
/// compute_NN tokens. `lto_NN` entries name LTO intermediates, not
/// architectures, and are ignored.
void collect_archs(llvm::StringRef spec, llvm::SmallVectorImpl<ArchToken>& out) {
    for(llvm::StringRef prefix: {"sm_", "compute_"}) {
        std::size_t pos = 0;
        while((pos = spec.find(prefix, pos)) != llvm::StringRef::npos) {
            pos += prefix.size();
            auto rest = spec.substr(pos);

            unsigned number = 0;
            std::size_t len = 0;
            while(len < rest.size() && llvm::isDigit(rest[len])) {
                number = number * 10 + (rest[len] - '0');
                len += 1;
            }
            if(len == 0)
                continue;

            char suffix = 0;
            if(len < rest.size() && llvm::isAlpha(rest[len]))
                suffix = rest[len];

            out.push_back({number, suffix});
        }
    }
}

/// Only the `arch=` clause of a -gencode value names the virtual
/// architecture the device front end compiles for; `code=` entries are
/// ptxas targets (`-gencode arch=compute_80,code=sm_90` preprocesses with
/// `__CUDA_ARCH__` == 800).
void collect_gencode_archs(llvm::StringRef spec, llvm::SmallVectorImpl<ArchToken>& out) {
    llvm::SmallVector<llvm::StringRef> pieces;
    spec.split(pieces, ',', -1, false);
    for(llvm::StringRef piece: pieces) {
        piece = piece.trim();
        if(piece.consume_front("arch="))
            collect_archs(piece, out);
    }
}

/// The newest architecture wins; at equal number the fuller feature set does
/// ('a' unlocks the arch-specific instructions the family 'f' and plain
/// forms hide, e.g. sm_90a for Hopper GMMA/TMA).
std::pair<unsigned, int> arch_rank(const ArchToken& arch) {
    int suffix_rank = arch.suffix == 'a' ? 2 : arch.suffix == 'f' ? 1 : 0;
    return {arch.number, suffix_rank};
}

std::string select_gpu_arch(llvm::ArrayRef<ArchToken> archs) {
    auto best = *std::ranges::max_element(archs, {}, arch_rank);

    auto result = std::format("sm_{}", best.number);
    if(best.suffix != 0)
        result += best.suffix;
    return result;
}

/// nvcc resolves relative paths against its working directory — the compile
/// directory — not against ours. A leading slash is absolute on POSIX and
/// root-relative on Windows: never compile-directory-relative.
std::string absolutize(llvm::StringRef value, llvm::StringRef directory) {
    if(value.empty() || directory.empty() || path::is_absolute(value) || value.front() == '/')
        return value.str();
    return path::join(directory, value);
}

/// Split on unescaped commas, `\,` reading as a literal comma. Other
/// backslashes stay verbatim: they are path separators on Windows, and
/// nvcc's fuller Linux-side escape processing (a backslash escapes any next
/// character) would destroy every native path.
void split_list(llvm::StringRef value, llvm::SmallVectorImpl<std::string>& out) {
    std::string piece;
    for(std::size_t i = 0; i < value.size(); i += 1) {
        char c = value[i];
        if(c == '\\' && i + 1 < value.size() && value[i + 1] == ',') {
            piece += ',';
            i += 1;
            continue;
        }
        if(c == ',') {
            if(!piece.empty())
                out.push_back(std::move(piece));
            piece.clear();
            continue;
        }
        piece += c;
    }
    if(!piece.empty())
        out.push_back(std::move(piece));
}

/// `--options-file` pulls additional nvcc arguments from a response file —
/// CMake routes long CUDA include lists through one — so dropping it would
/// lose every -I inside. Each pass splices all response files in place;
/// repeated passes resolve nested files, with a small cap as a cycle guard.
std::vector<std::string> expand_options_files(llvm::ArrayRef<const char*> arguments,
                                              llvm::StringRef directory) {
    std::vector<std::string> args(arguments.begin(), arguments.end());

    for(int depth = 0; depth < 4; depth += 1) {
        bool expanded = false;
        std::vector<std::string> next;
        next.reserve(args.size());

        for(std::size_t i = 0; i < args.size(); i += 1) {
            llvm::StringRef arg = args[i];
            llvm::StringRef value;

            if(arg == "-optf" || arg == "--options-file") {
                if(i + 1 < args.size()) {
                    i += 1;
                    value = args[i];
                }
            } else if(arg.consume_front("-optf=") || arg.consume_front("--options-file=")) {
                value = arg;
            } else {
                next.emplace_back(std::move(args[i]));
                continue;
            }

            expanded = true;
            /// The value is a comma-separated file list (`-optf a.rsp,b.rsp`
            /// reads both); a literal comma in a path arrives escaped as
            /// `\,`, so the split leaves native Windows paths intact.
            llvm::SmallVector<std::string> files;
            split_list(value, files);
            for(llvm::StringRef file: files) {
                auto file_path = absolutize(file, directory);
                auto buffer = llvm::MemoryBuffer::getFile(file_path);
                if(!buffer) {
                    LOG_WARN("Cannot read nvcc options file {}: {}",
                             file_path,
                             buffer.getError().message());
                    continue;
                }

                llvm::BumpPtrAllocator alloc;
                llvm::StringSaver saver(alloc);
                llvm::SmallVector<const char*> tokens;
                llvm::cl::TokenizeGNUCommandLine((*buffer)->getBuffer(), saver, tokens);
                for(llvm::StringRef token: tokens)
                    next.emplace_back(token);
            }
        }

        args = std::move(next);
        if(!expanded)
            break;
    }

    return args;
}

}  // namespace

bool is_nvcc_probe_flag(llvm::StringRef arg) {
    return arg.starts_with(ccbin_prefix) || arg == allow_unsupported_flag ||
           arg.starts_with(target_directory_prefix) || arg.starts_with(gpu_arch_prefix);
}

std::vector<std::string> translate_nvcc_command(llvm::ArrayRef<const char*> arguments,
                                                llvm::StringRef directory,
                                                bool edit) {
    std::vector<std::string> result;
    result.emplace_back(arguments[0]);

    /// Stateful nvcc options are last-wins (`-rdc=true -rdc=false` compiles
    /// without relocatable device code), so they collect here and render
    /// once at the end. -gencode is the exception: entries accumulate.
    llvm::SmallVector<ArchToken> gencode_archs;
    llvm::SmallVector<ArchToken> arch_archs;
    llvm::StringRef arch_special;
    std::string host_compiler;
    std::string target_directory;
    llvm::StringRef default_stream;
    llvm::StringRef machine;
    llvm::StringRef optimize;
    bool allow_unsupported = false;
    std::optional<bool> rdc;
    bool ewp = false;
    bool relaxed_constexpr = false;
    bool extended_lambda = false;
    bool device_debug = false;
    bool use_fast_math = false;

    auto args = expand_options_files(arguments.drop_front(), directory);

    /// Match `-opt value` / `-opt=value` for any of the spellings, advancing
    /// past a separate value. A spelling at the end of the command matches
    /// with an empty value.
    auto value_of = [&](std::size_t& i,
                        std::initializer_list<llvm::StringRef> spellings,
                        llvm::StringRef& value) {
        llvm::StringRef arg = args[i];
        for(auto spelling: spellings) {
            if(arg == spelling) {
                value = {};
                if(i + 1 < args.size()) {
                    i += 1;
                    value = args[i];
                }
                return true;
            }
            if(arg.starts_with(spelling) && arg[spelling.size()] == '=') {
                value = arg.substr(spelling.size() + 1);
                return true;
            }
        }
        return false;
    };

    /// nvcc treats every preprocessor value as a comma-separated list,
    /// short spellings included: `-Ia,b` names two directories.
    auto emit_list = [&](llvm::StringRef flag, llvm::StringRef value) {
        llvm::SmallVector<std::string> pieces;
        split_list(value, pieces);
        for(auto& piece: pieces) {
            result.emplace_back(flag);
            result.emplace_back(std::move(piece));
        }
    };

    for(std::size_t i = 0; i < args.size(); i += 1) {
        llvm::StringRef arg = args[i];
        llvm::StringRef value;

        if(value_of(i, {"-gencode", "--generate-code"}, value)) {
            collect_gencode_archs(value, gencode_archs);
            continue;
        }
        if(value_of(i, {"-arch", "--gpu-architecture"}, value)) {
            arch_archs.clear();
            arch_special = {};
            collect_archs(value, arch_archs);
            if(arch_archs.empty())
                arch_special = value;
            continue;
        }
        /// ptxas targets only — consumed so the value cannot leak, never
        /// harvested.
        if(value_of(i, {"-code", "--gpu-code"}, value)) {
            continue;
        }

        if(value_of(i, {"-ccbin", "--compiler-bindir"}, value)) {
            /// A bare program name resolves on PATH like nvcc would; only
            /// path-shaped values anchor to the compile directory.
            bool path_shaped =
                value.contains('/') || value.contains('\\') || value == "." || value == "..";
            host_compiler = path_shaped ? absolutize(value, directory) : value.str();
            continue;
        }
        if(arg == "--allow-unsupported-compiler" || arg == "-allow-unsupported-compiler") {
            allow_unsupported = true;
            continue;
        }
        if(value_of(i, {"-target-dir", "--target-directory"}, value)) {
            target_directory = value;
            continue;
        }

        /// nvcc packs several host flags into one comma-separated value,
        /// under the same `\,` escape as every other list option
        /// (`-Xcompiler=-Wl\,-z\,defs` reaches the host as one flag).
        if(value_of(i, {"-Xcompiler", "--compiler-options"}, value)) {
            llvm::SmallVector<std::string> pieces;
            split_list(value, pieces);
            for(auto& piece: pieces)
                result.emplace_back(std::move(piece));
            continue;
        }

        if(value_of(i, {"-std", "--std"}, value)) {
            result.emplace_back(("-std=" + value).str());
            continue;
        }

        if(value_of(i, {"-x", "--x"}, value)) {
            result.emplace_back("-x");
            result.emplace_back(value == "cu" ? "cuda" : value.str());
            continue;
        }

        /// The joined short forms -m32/-m64 are nvcc-fatal and fall through
        /// below as clang's own spellings; a value nvcc rejects pins
        /// nothing.
        if(value_of(i, {"-m", "--machine"}, value)) {
            if(value == "32" || value == "64")
                machine = value;
            continue;
        }

        if(value_of(i, {"-O", "--optimize"}, value)) {
            optimize = value;
            continue;
        }
        /// nvcc reads joined -O3 as its own option, not host passthrough.
        if(arg.size() > 2 && arg.starts_with("-O") &&
           std::ranges::all_of(arg.drop_front(2), llvm::isDigit)) {
            optimize = arg.drop_front(2);
            continue;
        }

        if(arg == "--disable-warnings" || arg == "-disable-warnings") {
            result.emplace_back("-w");
            continue;
        }

        if(value_of(i, {"-rdc", "--relocatable-device-code"}, value)) {
            rdc = value == "true";
            continue;
        }
        /// Separate compilation implies -rdc=true.
        if(arg == "-dc" || arg == "--device-c") {
            rdc = true;
            continue;
        }
        if(arg == "-ewp" || arg == "--extensible-whole-program") {
            ewp = true;
            continue;
        }

        if(value_of(i, {"-default-stream", "--default-stream"}, value)) {
            default_stream = value;
            continue;
        }

        /// Feature toggles whose only parse-visible effect is a macro.
        if(arg == "--expt-relaxed-constexpr" || arg == "-expt-relaxed-constexpr") {
            relaxed_constexpr = true;
            continue;
        }
        if(arg == "--expt-extended-lambda" || arg == "-expt-extended-lambda" ||
           arg == "--extended-lambda" || arg == "-extended-lambda") {
            extended_lambda = true;
            continue;
        }
        /// Consumed rather than passed through: clang reads a bare -G as
        /// its small-data-threshold option.
        if(arg == "-G" || arg == "--device-debug") {
            device_debug = true;
            continue;
        }
        /// Device fast math has no preprocess-visible effect in nvcc (cicc
        /// swaps the transcendentals itself); clang's flag selects the same
        /// fast variants in its math wrapper via
        /// __CLANG_GPU_APPROX_TRANSCENDENTALS__.
        if(arg == "--use_fast_math" || arg == "-use_fast_math") {
            use_fast_math = true;
            continue;
        }

        /// Preprocessor list options, rewritten to the short spellings the
        /// CDB classification knows. The exact-spelling matches must come
        /// before the direct-join ones: `-include` also starts with "-I".
        if(value_of(i, {"-isystem", "--system-include"}, value)) {
            emit_list("-isystem", value);
            continue;
        }
        if(value_of(i, {"-include", "--pre-include"}, value)) {
            emit_list("-include", value);
            continue;
        }
        if(arg.size() > 2 && arg.starts_with("-I") && arg[2] != '=') {
            emit_list("-I", arg.substr(2));
            continue;
        }
        if(value_of(i, {"-I", "--include-path"}, value)) {
            emit_list("-I", value);
            continue;
        }
        if(arg.size() > 2 && arg.starts_with("-D") && arg[2] != '=') {
            emit_list("-D", arg.substr(2));
            continue;
        }
        if(value_of(i, {"-D", "--define-macro"}, value)) {
            emit_list("-D", value);
            continue;
        }
        if(arg.size() > 2 && arg.starts_with("-U") && arg[2] != '=') {
            emit_list("-U", arg.substr(2));
            continue;
        }
        if(value_of(i, {"-U", "--undefine-macro"}, value)) {
            emit_list("-U", value);
            continue;
        }

        /// Pass-through wrappers for other tools: dropped together with the
        /// value, which could otherwise be mistaken for a host flag
        /// (`-Xptxas -O3` sets the ptxas level, not the host one).
        if(value_of(i,
                    {"-Xptxas",
                     "--ptxas-options",
                     "-Xnvlink",
                     "--nvlink-options",
                     "-Xfatbin",
                     "--fatbin-options",
                     "-Xarchive",
                     "--archive-options",
                     "-Xlinker",
                     "--linker-options",
                     "-Xcudafe",
                     "--threads",
                     "-t"},
                    value)) {
            continue;
        }

        result.emplace_back(arg);
    }

    /// nvcc injects its macros ahead of the user's preprocessor flags, so a
    /// later `-U` can undo them (`-dc -U__CUDACC_RDC__` parses without the
    /// RDC macro) — the synthetic ones render first.
    std::vector<std::string> prelude;
    if(rdc == true) {
        prelude.emplace_back("-fgpu-rdc");
        /// nvcc defines it; clang's -fgpu-rdc does not.
        prelude.emplace_back("-D__CUDACC_RDC__");
    } else if(edit && rdc == false) {
        prelude.emplace_back("-fno-gpu-rdc");
        prelude.emplace_back("-U__CUDACC_RDC__");
    }
    if(relaxed_constexpr)
        prelude.emplace_back("-D__CUDACC_RELAXED_CONSTEXPR__");
    if(extended_lambda)
        prelude.emplace_back("-D__CUDACC_EXTENDED_LAMBDA__");
    if(ewp)
        prelude.emplace_back("-D__CUDACC_EWP__");
    if(device_debug)
        prelude.emplace_back("-D__CUDACC_DEBUG__");
    if(use_fast_math)
        prelude.emplace_back("-fgpu-approx-transcendentals");
    /// The stream cancellation also renders ahead of the segment's own
    /// flags: the base's injected macro sits before the seam either way,
    /// and an explicit -D of the macro inside this edit must survive the
    /// -U, like it survives nvcc's absent injection.
    if(edit && !default_stream.empty() && default_stream != "per-thread")
        prelude.emplace_back("-UCUDA_API_PER_THREAD_DEFAULT_STREAM");
    result.insert(result.begin() + 1, prelude.begin(), prelude.end());

    /// The stream macro is nvcc's one exception: it lands after the user's
    /// preprocessor flags.
    if(default_stream == "per-thread")
        result.emplace_back("-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1");

    /// nvcc's own -O/-m always land after the -Xcompiler payloads on the
    /// host line, beating them regardless of input order.
    if(!machine.empty())
        result.emplace_back(("-m" + machine).str());
    if(!optimize.empty())
        result.emplace_back(("-O" + optimize).str());

    /// nvcc compiles the union of the -gencode entries and the (last-wins)
    /// -arch choice, one device pass each — the newest of the union is the
    /// view. A non-numeric -arch next to -gencode entries joins nvcc's
    /// union too, but resolving it needs the dryrun; the numeric side
    /// approximates the pick.
    llvm::SmallVector<ArchToken> archs = gencode_archs;
    archs.append(arch_archs.begin(), arch_archs.end());
    if(!archs.empty()) {
        /// A pure -arch edit replaces the base's architectures: nvcc itself
        /// would union it with the base's -gencode entries, but an appended
        /// -arch reads as the user picking the view outright. -gencode
        /// edits accumulate like nvcc's own, emitted bare for
        /// collapse_gpu_arch_flags to resolve against the base's.
        if(edit && gencode_archs.empty())
            result.emplace_back("--no-offload-arch=all");
        result.emplace_back("--cuda-gpu-arch=" + select_gpu_arch(archs));
    } else if(!arch_special.empty()) {
        /// A non-numeric selection (native, all, all-major) only nvcc can
        /// resolve: it persists as a probe token, the dryrun runs with it,
        /// and its cicc line then names the concrete architecture.
        if(edit)
            result.emplace_back("--no-offload-arch=all");
        result.emplace_back((llvm::Twine(gpu_arch_prefix) + arch_special).str());
    }

    if(!host_compiler.empty())
        result.emplace_back((llvm::Twine(ccbin_prefix) + host_compiler).str());
    if(allow_unsupported)
        result.emplace_back(allow_unsupported_flag.str());
    if(!target_directory.empty())
        result.emplace_back((llvm::Twine(target_directory_prefix) + target_directory).str());

    return result;
}

std::optional<llvm::SmallVector<std::size_t>>
    collapse_gpu_archs(llvm::ArrayRef<std::pair<ArchFlagKind, llvm::StringRef>> sequence) {
    struct ActiveArch {
        std::size_t index;
        llvm::SmallVector<ArchToken, 1> tokens;
        std::pair<unsigned, int> rank;
    };

    /// The arch flags still alive under clang's accumulate semantics:
    /// --no-offload-arch=all erases everything before it, a specific
    /// --no-offload-arch=sm_NN only its matches.
    llvm::SmallVector<ActiveArch> active;
    for(std::size_t i = 0; i < sequence.size(); i += 1) {
        auto [kind, value] = sequence[i];
        if(kind == ArchFlagKind::NoOffloadArch) {
            if(value == "all") {
                active.clear();
                continue;
            }
            llvm::SmallVector<ArchToken> removed;
            collect_archs(value, removed);
            if(removed.empty())
                return std::nullopt;
            auto matched = [&](const ArchToken& token) {
                return std::ranges::contains(removed, token);
            };
            for(std::size_t j = active.size(); j > 0;) {
                j -= 1;
                auto& entry = active[j];
                if(std::ranges::none_of(entry.tokens, matched))
                    continue;
                /// A flag naming both erased and surviving architectures
                /// cannot drop at flag granularity — bail like below.
                if(!std::ranges::all_of(entry.tokens, matched))
                    return std::nullopt;
                active.erase(active.begin() + j);
            }
            continue;
        }

        llvm::SmallVector<ArchToken> tokens;
        collect_archs(value, tokens);
        /// A value without an sm_NN/compute_NN token (a raw clang spelling
        /// like --offload-arch=native in a config append) is outside the
        /// ranking — leave the whole command to clang's own semantics.
        if(tokens.empty())
            return std::nullopt;
        auto rank = arch_rank(*std::ranges::max_element(tokens, {}, arch_rank));
        active.push_back({.index = i, .tokens = std::move(tokens), .rank = rank});
    }
    if(active.size() < 2)
        return llvm::SmallVector<std::size_t>{};

    auto best = std::ranges::max(active, {}, &ActiveArch::rank).rank;
    llvm::SmallVector<std::size_t> dropped;
    for(auto& arch: active) {
        if(arch.rank < best)
            dropped.push_back(arch.index);
    }
    return dropped;
}

std::expected<NVCCDryrunInfo, std::string> parse_nvcc_dryrun(llvm::StringRef output) {
    NVCCDryrunInfo info;
    llvm::StringRef nvvm_dir;
    llvm::SmallVector<ArchToken> cicc_archs;
    llvm::SmallVector<const char*, 64> first_command;

    auto harvest_defines = [](llvm::ArrayRef<const char*> tokens, std::vector<std::string>& out) {
        for(llvm::StringRef token: tokens) {
            if(!token.consume_front("-D"))
                continue;
            auto name = token.substr(0, token.find('='));
            if(name == "__CUDACC__" || name == "__CUDA_ARCH__" || name == "__CUDA_ARCH_LIST__")
                continue;
            out.emplace_back(token);
        }
    };

    llvm::BumpPtrAllocator alloc;
    llvm::StringSaver saver(alloc);

    llvm::SmallVector<llvm::StringRef> lines;
    output.split(lines, '\n', -1, false);

    for(llvm::StringRef line: lines) {
        line = line.trim();
        if(!line.consume_front("#$ "))
            continue;

        if(line.consume_front("TOP=")) {
            info.cuda_path = line.trim();
            continue;
        }

        if(line.consume_front("NVVMIR_LIBRARY_DIR=")) {
            nvvm_dir = line.trim();
            continue;
        }

        if(line.consume_front("PATH=")) {
            llvm::SmallVector<llvm::StringRef> dirs;
            line.split(dirs, llvm::sys::EnvPathSeparator, -1, false);
            for(auto dir: dirs)
                info.search_path.emplace_back(dir);
            continue;
        }

        /// Remaining lines are either environment assignments (INCLUDES=...,
        /// CICC_PATH=..., no space before '=') or pipeline commands.
        auto head = line.take_until([](char c) { return c == ' '; });
        if(head.contains('='))
            continue;

        llvm::SmallVector<const char*, 64> tokens;
        llvm::cl::TokenizeGNUCommandLine(line, saver, tokens);
        if(tokens.empty())
            continue;

        llvm::StringRef argv0 = tokens[0];
        auto program = path::filename(argv0);

        if(program == "cudafe++") {
            for(llvm::StringRef token: tokens) {
                if(token.starts_with("--c++")) {
                    info.cpp_dialect = token.substr(2);
                    break;
                }
            }
            continue;
        }

        /// A probe carrying `-arch=all` runs one cicc per architecture:
        /// collect them all and pick by the same newest-wins policy as the
        /// translation.
        if(program == "cicc") {
            for(std::size_t i = 0; i + 1 < tokens.size(); i += 1) {
                if(llvm::StringRef(tokens[i]) == "-arch")
                    collect_archs(tokens[i + 1], cicc_archs);
            }
            continue;
        }

        auto tail = llvm::ArrayRef(tokens).drop_front();

        if(first_command.empty())
            first_command.assign(tokens.begin(), tokens.end());

        bool is_preprocess = std::ranges::contains(tail, llvm::StringRef("-E"));
        if(!is_preprocess)
            continue;

        bool is_device = std::ranges::any_of(tail, [](llvm::StringRef token) {
            return token.starts_with("-D__CUDA_ARCH__");
        });

        if(info.host_compiler.empty())
            info.host_compiler = argv0;

        harvest_defines(tail, is_device ? info.device_defines : info.host_defines);
    }

    /// A dryrun without `TOP=` still names the toolkit root indirectly:
    /// `NVVMIR_LIBRARY_DIR=` is `<root>/nvvm/libdevice`.
    if(info.cuda_path.empty() && !nvvm_dir.empty())
        info.cuda_path = path::parent_path(path::parent_path(nvvm_dir));

    if(!cicc_archs.empty())
        info.default_arch = select_gpu_arch(cicc_archs);

    /// A host-language input (`nvcc -c foo.cpp`) compiles in a single host
    /// step with no preprocess stage: the first pipeline command is the
    /// host compile line. Its defines are kept unfiltered — outside CUDA
    /// mode clang derives none of them (nvcc injects even
    /// __CUDA_ARCH_LIST__ there).
    if(info.host_compiler.empty() && !first_command.empty()) {
        info.host_compiler = first_command[0];
        for(llvm::StringRef token: llvm::ArrayRef(first_command).drop_front()) {
            if(token.consume_front("-D"))
                info.host_defines.emplace_back(token);
        }
    }

    if(info.host_compiler.empty())
        return std::unexpected("nvcc dryrun output has no host compile command");

    return info;
}

}  // namespace clice
