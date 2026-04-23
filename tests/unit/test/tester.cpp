#include "test/tester.h"

#include <cassert>
#include <format>

#include "syntax/scan.h"

namespace clice::testing {

namespace {

std::vector<std::string> base_cc1_args(llvm::StringRef standard) {
    return {
        "clang",
        "-cc1",
        "-triple",
        LLVM_DEFAULT_TARGET_TRIPLE,
        standard.str(),
        "-ffreestanding",
        "-undef",
        "-fms-extensions",
        "-fsyntax-only",
        "-x",
        "c++",
    };
}

}  // namespace

Tester::~Tester() {
    for(auto& path: pcm_paths) {
        fs::remove(path);
    }
}

bool Tester::try_compile() {
    auto built = clice::compile(params);
    if(!built.completed()) {
        for(auto& diag: built.diagnostics()) {
            LOG_ERROR("{}", diag.message);
        }
        return false;
    }
    unit.emplace(std::move(built));
    return true;
}

void Tester::prepare(llvm::StringRef standard) {
    params = CompilationParams();
    unit.reset();
    vfs = llvm::makeIntrusiveRefCnt<TestVFS>();

    for(auto& [file, source]: sources.all_files) {
        vfs->add(file, source.content);
    }

    owned_args = base_cc1_args(standard);
    owned_args.push_back(TestVFS::path(src_path));

    params.arguments.clear();
    for(auto& arg: owned_args) {
        params.arguments.push_back(arg.c_str());
    }

    params.kind = CompilationKind::Content;
    params.vfs = vfs;
}

bool Tester::compile(llvm::StringRef standard) {
    prepare(standard);
    return try_compile();
}

bool Tester::compile_with_pch(llvm::StringRef standard) {
    prepare(standard);

    auto pch_path = fs::createTemporaryFile("clice", "pch");
    if(!pch_path) {
        LOG_ERROR("{}", pch_path.error().message());
        return false;
    }

    auto overlay =
        llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    overlay->pushOverlay(vfs);
    params.vfs = overlay;

    // Phase 1: Build PCH from the preamble portion.
    params.kind = CompilationKind::Preamble;
    params.output_file = *pch_path;

    auto& main_source = sources.all_files[src_path];
    auto bound = compute_preamble_bound(main_source.content);
    auto main_vfs_path = TestVFS::path(src_path);
    params.add_remapped_file(main_vfs_path, main_source.content, bound);

    PCHInfo info;
    {
        auto preamble_unit = clice::compile(params, info);
        if(!preamble_unit.completed()) {
            for(auto& diag: preamble_unit.diagnostics()) {
                LOG_ERROR("{}", diag.message);
            }
            return false;
        }
    }

    // Phase 2: Compile content using the PCH.
    params.output_file.clear();
    params.kind = CompilationKind::Content;
    params.pch = {info.path, static_cast<std::uint32_t>(info.preamble.size())};
    params.buffers.clear();

    return try_compile();
}

bool Tester::compile_with_modules(llvm::StringRef standard) {
    std::vector<ModuleFile> all_modules = module_files;
    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            continue;
        }
        auto result = scan(source.content);
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

    auto scan_args_base = base_cc1_args(standard);

    std::vector<ScannedModule> modules;
    for(auto& mod: all_modules) {
        auto args = scan_args_base;
        args.push_back(TestVFS::path(mod.filename));

        std::vector<const char*> argv;
        for(auto& arg: args) {
            argv.push_back(arg.c_str());
        }

        auto result = scan_precise(argv, TestVFS::root(), {}, nullptr, vfs);
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

    auto overlay =
        llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    overlay->pushOverlay(vfs);

    llvm::StringMap<std::string> built_pcms;
    for(auto idx: order) {
        auto& mod = modules[idx];

        auto pcm_path = fs::createTemporaryFile("clice", "pcm");
        if(!pcm_path) {
            LOG_ERROR("{}", pcm_path.error().message());
            return false;
        }
        pcm_paths.push_back(*pcm_path);

        Tester builder;
        builder.add_main(mod.filename, mod.content);
        builder.prepare(standard);
        builder.params.kind = CompilationKind::ModuleInterface;
        builder.params.output_file = *pcm_path;
        builder.params.vfs = overlay;
        builder.params.pcms = built_pcms;

        PCMInfo info;
        auto built = clice::compile(builder.params, info);
        if(!built.completed()) {
            for(auto& diag: built.diagnostics()) {
                LOG_ERROR("{}", diag.message);
            }
            return false;
        }

        built_pcms.try_emplace(mod.module_name, info.path);
    }

    prepare(standard);
    params.vfs = overlay;
    params.pcms = std::move(built_pcms);
    return try_compile();
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
    params = CompilationParams();
    unit.reset();
    vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    for(auto& [file, source]: sources.all_files) {
        vfs->add(file, source.content);
    }

    auto command = std::format("clang++ {} {} -fms-extensions", standard, src_path);
    database.add_command("fake", src_path, command);

    CommandOptions options;
    options.query_toolchain = true;
    options.suppress_logging = true;
    auto commands = database.lookup(src_path, options);
    assert(!commands.empty() && "lookup failed after add_command");
    params.arguments = commands.front().to_argv();

    params.kind = CompilationKind::Content;

    auto overlay =
        llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    overlay->pushOverlay(vfs);
    params.vfs = overlay;

    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            params.add_remapped_file(file, source.content);
        } else {
            std::string path = path::is_absolute(file) ? file.str() : path::join(".", file);
            params.add_remapped_file(path, source.content);
        }
    }
}

bool Tester::compile_driver(llvm::StringRef standard) {
    prepare_driver(standard);
    return try_compile();
}

bool Tester::compile_driver_with_pch(llvm::StringRef standard) {
    prepare_driver(standard);

    auto pch_path = fs::createTemporaryFile("clice", "pch");
    if(!pch_path) {
        LOG_ERROR("{}", pch_path.error().message());
        return false;
    }

    // Phase 1: Build PCH from the preamble portion.
    params.kind = CompilationKind::Preamble;
    params.output_file = *pch_path;

    // Clear buffers from prepare_driver() so we can re-add with preamble bound.
    params.buffers.clear();
    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            auto bound = compute_preamble_bound(source.content);
            params.add_remapped_file(file, source.content, bound);
        } else {
            std::string path = path::is_absolute(file) ? file.str() : path::join(".", file);
            params.add_remapped_file(path, source.content);
        }
    }

    PCHInfo info;
    {
        auto preamble_unit = clice::compile(params, info);
        if(!preamble_unit.completed()) {
            for(auto& diag: preamble_unit.diagnostics()) {
                LOG_ERROR("{}", diag.message);
            }
            return false;
        }
    }

    // Phase 2: Compile content using the PCH.
    params.output_file.clear();
    params.kind = CompilationKind::Content;
    params.pch = {info.path, static_cast<std::uint32_t>(info.preamble.size())};
    params.buffers.clear();

    return try_compile();
}

void Tester::clear() {
    params = CompilationParams();
    database = CompilationDatabase();
    unit.reset();
    sources.all_files.clear();
    src_path.clear();
    owned_args.clear();
    vfs.reset();
    module_files.clear();
    for(auto& path: pcm_paths) {
        fs::remove(path);
    }
    pcm_paths.clear();
}

}  // namespace clice::testing
