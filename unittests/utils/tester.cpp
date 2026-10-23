module;

#include "support/prelude.h"

#ifdef _WIN32
// See cache_store.cpp: windows.h must not spill min/max macros.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include "support/logging.macros.h"

module clice;

import :command.table;
import :support.logging;
import :syntax.preamble;
import :syntax.scan;
import :unittests.utils.temp_dir;
import :unittests.utils.tester;
import :vfs.file_system;

namespace clice::testing {

namespace {

/// The language follows the standard: `-std=c17` compiles C.
std::vector<std::string> base_cc1_args(llvm::StringRef standard, llvm::StringRef triple) {
    bool cxx = standard.contains("++");
    return {
        "clang",
        "-cc1",
        "-triple",
        triple.empty() ? LLVM_DEFAULT_TARGET_TRIPLE : triple.str(),
        standard.str(),
        "-ffreestanding",
        "-undef",
        "-fms-extensions",
        "-fsyntax-only",
        "-x",
        cxx ? "c++" : "c",
    };
}

}  // namespace

bool Tester::try_compile() {
    auto built = clice::compile(std::move(params));
    if(!built->completed()) {
        for(auto& diag: built->diagnostics()) {
            LOG_ERROR("{}", diag.message);
        }
        return false;
    }
    unit = std::move(built);
    return true;
}

void Tester::prepare(llvm::StringRef standard) {
    params = CompilationParams{.kind = kind};
    unit.reset();
    vfs = llvm::makeIntrusiveRefCnt<TestVFS>();

    for(auto& [file, source]: sources.all_files) {
        vfs->add(file, source.content);
    }

    params.input.arguments = base_cc1_args(standard, triple);
    params.input.arguments.push_back(TestVFS::path(src_path));
    params.input.vfs = vfs;
}

bool Tester::compile(llvm::StringRef standard) {
    prepare(standard);
    return try_compile();
}

bool Tester::compile_with_pch(llvm::StringRef standard) {
    prepare(standard);

    auto pch_path = artifact("pch");

    auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
        llvm::makeIntrusiveRefCnt<vfs::View>());
    overlay->pushOverlay(vfs);
    params.input.vfs = overlay;

    // Phase 1: Build PCH from the preamble portion.
    auto& focus_source = sources.all_files[src_path];
    auto bound = preamble_bound(focus_source.content);
    CompilationParams preamble{
        .kind = CompilationKind::Preamble,
        .input = params.input,
        .output_file = pch_path,
    };
    preamble.input.add_remapped_file(TestVFS::path(src_path), focus_source.content, bound);
    {
        auto preamble_unit = clice::compile(std::move(preamble));
        if(!preamble_unit->has_output()) {
            for(auto& diag: preamble_unit->diagnostics()) {
                LOG_ERROR("{}", diag.message);
            }
            return false;
        }
    }

    // Phase 2: Compile content using the PCH. Content compiles read the
    // sources through remapping too.
    params.input.pch = {pch_path, bound};
    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            params.input.add_focus(file, source.content);
        } else {
            std::string path = path::is_absolute(file) ? file.str() : path::join(".", file);
            params.input.add_remapped_file(path, source.content);
        }
    }

    return try_compile();
}

bool Tester::compile_with_modules(llvm::StringRef standard) {
    std::vector<ModuleFile> all_modules = module_files;
    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            continue;
        }
        auto result = scan_quick(source.content);
        if(!result.module_name.empty() || result.need_preprocess) {
            all_modules.push_back({file.str(), source.content});
        }
    }

    if(all_modules.empty()) {
        return compile(standard);
    }

    vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    for(auto& [file, source]: sources.all_files) {
        vfs->add(file, source.content);
    }
    for(auto& mod: module_files) {
        vfs->add(mod.filename, mod.content);
    }

    struct ScannedModule {
        std::string filename;
        std::string content;
        std::string module_name;
        std::vector<std::string> deps;
    };

    auto scan_args_base = base_cc1_args(standard, triple);

    std::vector<ScannedModule> modules;
    for(auto& mod: all_modules) {
        auto args = scan_args_base;
        args.push_back(TestVFS::path(mod.filename));

        std::vector<const char*> argv;
        for(auto& arg: args) {
            argv.push_back(arg.c_str());
        }

        auto result = scan_precise(argv, TestVFS::root(), {}, vfs);
        if(result.provided_module().empty()) {
            continue;
        }
        modules.push_back(
            {mod.filename, mod.content, result.module_name, std::move(result.modules)});
    }

    llvm::StringMap<std::size_t> name_to_index;
    for(std::size_t i = 0; i < modules.size(); ++i) {
        name_to_index[modules[i].module_name] = i;
    }

    std::vector<std::size_t> order;
    std::vector<int> state(modules.size(), 0);

    auto topo_visit = [&](this auto& self, std::size_t i) -> bool {
        if(state[i] == 2)
            return true;
        if(state[i] == 1) {
            LOG_ERROR("Circular module dependency involving {}", modules[i].module_name);
            return false;
        }
        state[i] = 1;
        for(auto& dep: modules[i].deps) {
            auto it = name_to_index.find(dep);
            if(it != name_to_index.end()) {
                if(!self(it->second))
                    return false;
            }
        }
        state[i] = 2;
        order.push_back(i);
        return true;
    };

    for(std::size_t i = 0; i < modules.size(); ++i) {
        if(!topo_visit(i))
            return false;
    }

    auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
        llvm::makeIntrusiveRefCnt<vfs::View>());
    overlay->pushOverlay(vfs);

    llvm::StringMap<std::string> built_pcms;
    for(auto idx: order) {
        auto& mod = modules[idx];

        auto pcm_path = artifact("pcm");

        Tester builder;
        builder.add_focus(mod.filename, mod.content);
        builder.prepare(standard);
        builder.params.kind = CompilationKind::ModuleInterface;
        builder.params.output_file = pcm_path;
        builder.params.input.vfs = overlay;
        builder.params.input.pcms = built_pcms;

        auto built = clice::compile(std::move(builder.params));
        if(!built->has_output()) {
            for(auto& diag: built->diagnostics()) {
                LOG_ERROR("{}", diag.message);
            }
            return false;
        }

        built_pcms.try_emplace(mod.module_name, pcm_path);
    }

    prepare(standard);
    params.input.vfs = overlay;
    params.input.pcms = std::move(built_pcms);
    if(!try_compile()) {
        return false;
    }

    // The AST builds even for broken sources, so a "completed" compile can
    // still have dropped whole subtrees (an unresolved import, a discarded
    // initializer). Module tests assert semantics across module boundaries;
    // running them against such an AST proves nothing, so fail loudly.
    for(auto& diag: unit->diagnostics()) {
        if(diag.id.level >= DiagnosticLevel::Error) {
            LOG_ERROR("module test compile has errors: {}", diag.message);
            return false;
        }
    }
    return true;
}

std::uint32_t Tester::point(llvm::StringRef name, llvm::StringRef file) {
    if(file.empty()) {
        file = src_path;
    }

    auto& offsets = sources.all_files[file].offsets;
    if(name.empty()) {
        assert(offsets.size() == 1);
        return offsets.begin()->second;
    }

    assert(offsets.contains(name));
    return offsets.lookup(name);
}

llvm::ArrayRef<std::uint32_t> Tester::nameless_points(llvm::StringRef file) {
    if(file.empty()) {
        file = src_path;
    }

    return sources.all_files[file].nameless_offsets;
}

LocalSourceRange Tester::range(llvm::StringRef name, llvm::StringRef file) {
    if(file.empty()) {
        file = src_path;
    }

    auto& ranges = sources.all_files[file].ranges;
    if(name.empty()) {
        assert(ranges.size() == 1);
        return ranges.begin()->second;
    }

    assert(ranges.contains(name));
    return ranges.lookup(name);
}

void Tester::prepare_driver(llvm::StringRef standard) {
    params = CompilationParams{.kind = kind};
    unit.reset();
    vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    for(auto& [file, source]: sources.all_files) {
        vfs->add(file, source.content);
    }

    auto command = std::format("clang++ {} {} -fms-extensions", standard, src_path);
    auto entry = database.add_command(TestVFS::root(), src_path, command);
    assert(entry && "no entry after add_command");
    CommandRef ref{entry->file,
                   entry->command,
                   commands.input_kind(entry->command, src_path),
                   CommandSource::CDBExact};
    auto rendered = toolchain.render(ref, database.spelling(ref.file)).argv;
    params.input.arguments.assign(rendered.begin(), rendered.end());

    auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
        llvm::makeIntrusiveRefCnt<vfs::View>());
    overlay->pushOverlay(vfs);
    params.input.vfs = overlay;

    Spelling focus(file_table.resolve(entry->file));
    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            params.input.add_focus(focus, source.content);
        } else {
            params.input.add_remapped_file(Spelling(file, focus.parent()), source.content);
        }
    }
}

bool Tester::compile_driver(llvm::StringRef standard) {
    prepare_driver(standard);
    return try_compile();
}

bool set_file_mtime(llvm::StringRef path, std::int64_t mtime_ns) {
#ifdef _WIN32
    // Path-based instead of fd-based: opening a directory needs
    // FILE_FLAG_BACKUP_SEMANTICS, which no LLVM open wrapper passes.
    std::wstring wide;
    if(!llvm::ConvertUTF8toWide(path, wide)) {
        return false;
    }
    HANDLE handle = ::CreateFileW(wide.c_str(),
                                  FILE_WRITE_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if(handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    // FILETIME counts 100ns intervals since 1601-01-01.
    std::uint64_t intervals = static_cast<std::uint64_t>(mtime_ns / 100) + 116444736000000000ull;
    FILETIME time;
    time.dwLowDateTime = static_cast<DWORD>(intervals);
    time.dwHighDateTime = static_cast<DWORD>(intervals >> 32);
    bool ok = ::SetFileTime(handle, nullptr, &time, &time);
    ::CloseHandle(handle);
    return ok;
#else
    timespec times[2];
    times[0].tv_sec = mtime_ns / 1'000'000'000;
    times[0].tv_nsec = mtime_ns % 1'000'000'000;
    times[1] = times[0];
    std::string buffer(path);
    return ::utimensat(AT_FDCWD, buffer.c_str(), times, 0) == 0;
#endif
}

void Tester::clear() {
    params = CompilationParams();
    unit.reset();
    sources.all_files.clear();
    src_path.clear();
    vfs.reset();
    module_files.clear();
}

}  // namespace clice::testing
