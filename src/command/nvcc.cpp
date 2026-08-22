#include "command/nvcc.h"

#include <algorithm>
#include <format>

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

/// One GPU architecture named inside an -arch/-gencode value.
struct ArchToken {
    unsigned number = 0;

    /// 'a' (arch-specific) or 'f' (family) variant, 0 for the plain form.
    char suffix = 0;
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
std::string select_gpu_arch(llvm::ArrayRef<ArchToken> archs) {
    auto rank = [](const ArchToken& arch) {
        auto suffix_rank = arch.suffix == 'a' ? 2 : arch.suffix == 'f' ? 1 : 0;
        return std::pair(arch.number, suffix_rank);
    };
    auto best = *std::ranges::max_element(archs, {}, rank);

    auto result = std::format("sm_{}", best.number);
    if(best.suffix != 0)
        result += best.suffix;
    return result;
}

/// nvcc resolves relative paths against its working directory — the compile
/// directory — not against ours.
std::string absolutize(llvm::StringRef value, llvm::StringRef directory) {
    if(value.empty() || directory.empty() || path::is_absolute(value))
        return value.str();
    return path::join(directory, value);
}

/// Split on unescaped commas. nvcc's backslash escapes the next character
/// unconditionally (`a\,b` is one value with a comma, `a\\,b` is `a\` and
/// `b`, `a\x` is `ax`).
void split_list(llvm::StringRef value, llvm::SmallVectorImpl<std::string>& out) {
    std::string piece;
    for(std::size_t i = 0; i < value.size(); i += 1) {
        char c = value[i];
        if(c == '\\' && i + 1 < value.size()) {
            piece += value[i + 1];
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
           arg.starts_with(target_directory_prefix);
}

std::vector<std::string> translate_nvcc_command(llvm::ArrayRef<const char*> arguments,
                                                llvm::StringRef directory) {
    std::vector<std::string> result;
    result.emplace_back(arguments[0]);

    /// Stateful nvcc options are last-wins (`-rdc=true -rdc=false` compiles
    /// without relocatable device code), so they collect here and render
    /// once at the end. -gencode is the exception: entries accumulate.
    llvm::SmallVector<ArchToken> gencode_archs;
    llvm::SmallVector<ArchToken> arch_archs;
    std::string host_compiler;
    std::string target_directory;
    llvm::StringRef default_stream;
    bool allow_unsupported = false;
    bool rdc = false;
    bool ewp = false;
    bool relaxed_constexpr = false;
    bool extended_lambda = false;

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
            collect_archs(value, arch_archs);
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

        /// nvcc packs several host flags into one comma-separated value.
        if(value_of(i, {"-Xcompiler", "--compiler-options"}, value)) {
            llvm::SmallVector<llvm::StringRef> pieces;
            value.split(pieces, ',', -1, false);
            for(auto piece: pieces)
                result.emplace_back(piece);
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
    if(rdc) {
        prelude.emplace_back("-fgpu-rdc");
        /// nvcc defines it; clang's -fgpu-rdc does not.
        prelude.emplace_back("-D__CUDACC_RDC__");
    }
    if(relaxed_constexpr)
        prelude.emplace_back("-D__CUDACC_RELAXED_CONSTEXPR__");
    if(extended_lambda)
        prelude.emplace_back("-D__CUDACC_EXTENDED_LAMBDA__");
    if(ewp)
        prelude.emplace_back("-D__CUDACC_EWP__");
    result.insert(result.begin() + 1, prelude.begin(), prelude.end());

    /// The stream macro is nvcc's one exception: it lands after the user's
    /// preprocessor flags.
    if(default_stream == "per-thread")
        result.emplace_back("-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1");

    /// nvcc rejects mixing -gencode with -arch, so at most one list is
    /// populated.
    auto& archs = gencode_archs.empty() ? arch_archs : gencode_archs;
    if(!archs.empty())
        result.emplace_back("--cuda-gpu-arch=" + select_gpu_arch(archs));

    if(!host_compiler.empty())
        result.emplace_back((llvm::Twine(ccbin_prefix) + host_compiler).str());
    if(allow_unsupported)
        result.emplace_back(allow_unsupported_flag.str());
    if(!target_directory.empty())
        result.emplace_back((llvm::Twine(target_directory_prefix) + target_directory).str());

    return result;
}

std::expected<NVCCDryrunInfo, std::string> parse_nvcc_dryrun(llvm::StringRef output) {
    NVCCDryrunInfo info;

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

        if(program == "cicc") {
            for(std::size_t i = 0; i + 1 < tokens.size(); i += 1) {
                llvm::StringRef token = tokens[i];
                llvm::StringRef arch = tokens[i + 1];
                if(token == "-arch" && arch.consume_front("compute_")) {
                    info.default_arch = ("sm_" + arch).str();
                    break;
                }
            }
            continue;
        }

        auto tail = llvm::ArrayRef(tokens).drop_front();
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

    if(info.cuda_path.empty())
        return std::unexpected("nvcc dryrun output has no TOP= line naming the toolkit root");
    if(info.host_compiler.empty())
        return std::unexpected("nvcc dryrun output has no host preprocess command");

    return info;
}

}  // namespace clice
