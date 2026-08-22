#include "command/nvcc.h"

#include <algorithm>
#include <format>

#include "support/filesystem.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/StringSaver.h"

namespace clice {

namespace {

/// One GPU architecture named inside an -arch/-code/-gencode value.
struct ArchToken {
    unsigned number = 0;

    /// 'a' (arch-specific) or 'f' (family) variant, 0 for the plain form.
    char suffix = 0;
};

/// Scan a spec like "arch=compute_90a,code=[compute_90a,sm_90a]" for
/// sm_NN / compute_NN tokens. `lto_NN` entries name LTO intermediates, not
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

}  // namespace

NVCCCommand translate_nvcc_command(llvm::ArrayRef<const char*> arguments) {
    NVCCCommand result;
    result.arguments.emplace_back(arguments[0]);

    llvm::SmallVector<ArchToken> archs;
    std::string host_compiler;

    auto args = arguments.drop_front();

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

    for(std::size_t i = 0; i < args.size(); i += 1) {
        llvm::StringRef arg = args[i];
        llvm::StringRef value;

        if(value_of(i,
                    {"-gencode",
                     "--generate-code",
                     "-arch",
                     "--gpu-architecture",
                     "-code",
                     "--gpu-code"},
                    value)) {
            collect_archs(value, archs);
            continue;
        }

        if(value_of(i, {"-ccbin", "--compiler-bindir"}, value)) {
            host_compiler = value;
            continue;
        }

        /// nvcc packs several host flags into one comma-separated value.
        if(value_of(i, {"-Xcompiler", "--compiler-options"}, value)) {
            llvm::SmallVector<llvm::StringRef> pieces;
            value.split(pieces, ',', -1, false);
            for(auto piece: pieces)
                result.arguments.emplace_back(piece);
            continue;
        }

        if(value_of(i, {"-std", "--std"}, value)) {
            result.arguments.emplace_back(("-std=" + value).str());
            continue;
        }

        if(value_of(i, {"-x", "--x"}, value)) {
            result.arguments.emplace_back("-x");
            result.arguments.emplace_back(value == "cu" ? "cuda" : value.str());
            continue;
        }

        if(value_of(i, {"-rdc", "--relocatable-device-code"}, value)) {
            if(value == "true") {
                result.arguments.emplace_back("-fgpu-rdc");
                /// nvcc defines it; clang's -fgpu-rdc does not.
                result.arguments.emplace_back("-D__CUDACC_RDC__");
            }
            continue;
        }

        if(value_of(i, {"-default-stream", "--default-stream"}, value)) {
            if(value == "per-thread")
                result.arguments.emplace_back("-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1");
            continue;
        }

        /// Feature toggles whose only parse-visible effect is a macro.
        if(arg == "--expt-relaxed-constexpr" || arg == "-expt-relaxed-constexpr") {
            result.arguments.emplace_back("-D__CUDACC_RELAXED_CONSTEXPR__");
            continue;
        }
        if(arg == "--expt-extended-lambda" || arg == "-expt-extended-lambda" ||
           arg == "--extended-lambda" || arg == "-extended-lambda") {
            result.arguments.emplace_back("-D__CUDACC_EXTENDED_LAMBDA__");
            continue;
        }

        /// Long-form preprocessor aliases, rewritten to the short spellings
        /// the CDB classification knows.
        if(value_of(i, {"--define-macro"}, value)) {
            result.arguments.emplace_back("-D");
            result.arguments.emplace_back(value);
            continue;
        }
        if(value_of(i, {"--undefine-macro"}, value)) {
            result.arguments.emplace_back("-U");
            result.arguments.emplace_back(value);
            continue;
        }
        if(value_of(i, {"--include-path"}, value)) {
            result.arguments.emplace_back("-I");
            result.arguments.emplace_back(value);
            continue;
        }
        if(value_of(i, {"--system-include", "-isystem"}, value)) {
            result.arguments.emplace_back("-isystem");
            result.arguments.emplace_back(value);
            continue;
        }
        if(value_of(i, {"--pre-include"}, value)) {
            result.arguments.emplace_back("-include");
            result.arguments.emplace_back(value);
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
                     "--options-file",
                     "-optf",
                     "--threads",
                     "-t"},
                    value)) {
            continue;
        }

        result.arguments.emplace_back(arg);
    }

    if(!archs.empty())
        result.arguments.emplace_back("--cuda-gpu-arch=" + select_gpu_arch(archs));

    if(!host_compiler.empty())
        result.arguments.emplace_back((llvm::Twine(nvcc_ccbin_prefix) + host_compiler).str());

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
