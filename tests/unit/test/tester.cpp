#include "test/tester.h"

#include <cassert>
#include <format>

#include "syntax/scan.h"

namespace clice::testing {

void Tester::prepare(llvm::StringRef standard) {
    params = CompilationParams();
    unit.reset();
    vfs = llvm::makeIntrusiveRefCnt<TestVFS>();

    for(auto& [file, source]: sources.all_files) {
        vfs->add(file, source.content);
    }

    owned_args.clear();
    // Use -cc1 mode directly to bypass the slow driver subprocess.
    owned_args.push_back("clang");
    owned_args.push_back("-cc1");
    owned_args.push_back("-triple");
    owned_args.push_back(LLVM_DEFAULT_TARGET_TRIPLE);
    owned_args.push_back(standard.str());
    owned_args.push_back("-ffreestanding");
    owned_args.push_back("-undef");
    owned_args.push_back("-fms-extensions");
    owned_args.push_back("-fsyntax-only");
    owned_args.push_back("-x");
    owned_args.push_back("c++");
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

bool Tester::compile_with_pch(llvm::StringRef standard) {
    prepare(standard);

    auto pch_path = fs::createTemporaryFile("clice", "pch");
    if(!pch_path) {
        LOG_ERROR("{}", pch_path.error().message());
        return false;
    }

    // Use an overlay VFS so the PCH temp file on real disk is accessible.
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
    params.arguments = database.lookup(src_path, options).front().arguments;

    params.kind = CompilationKind::Content;

    // Use overlay VFS: real FS (for system headers) + InMemoryFS (for test files).
    auto overlay =
        llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    overlay->pushOverlay(vfs);
    params.vfs = overlay;

    // Remap test files so clang sees our in-memory content.
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

bool Tester::compile_driver_with_pch(llvm::StringRef standard) {
    vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    for(auto& [file, source]: sources.all_files) {
        vfs->add(file, source.content);
    }

    auto command = std::format("clang++ {} {} -fms-extensions", standard, src_path);
    database.add_command("fake", src_path, command);

    CommandOptions options;
    options.query_toolchain = true;
    options.suppress_logging = true;
    params.arguments = database.lookup(src_path, options).front().arguments;

    auto pch_path = fs::createTemporaryFile("clice", "pch");
    if(!pch_path) {
        LOG_ERROR("{}", pch_path.error().message());
        return false;
    }

    // Use overlay VFS: real FS (for system headers + PCH temp) + InMemoryFS.
    auto overlay =
        llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    overlay->pushOverlay(vfs);
    params.vfs = overlay;

    // Phase 1: Build PCH from the preamble portion.
    params.kind = CompilationKind::Preamble;
    params.output_file = *pch_path;

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

    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            params.add_remapped_file(file, source.content);
        } else {
            std::string path = path::is_absolute(file) ? file.str() : path::join(".", file);
            params.add_remapped_file(path, source.content);
        }
    }

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

void Tester::clear() {
    params = CompilationParams();
    database = CompilationDatabase();
    unit.reset();
    sources.all_files.clear();
    src_path.clear();
    owned_args.clear();
    vfs.reset();
}

}  // namespace clice::testing
