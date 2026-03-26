#include "test/tester.h"

#include <cassert>
#include <format>

#include "syntax/scan.h"

namespace clice::testing {

void Tester::prepare(llvm::StringRef standard) {
    auto command = std::format("clang++ {} {} -fms-extensions", standard, src_path);

    database.add_command("fake", src_path, command);
    params.kind = CompilationKind::Content;

    CommandOptions options;
    options.query_toolchain = true;
    options.suppress_logging = true;

    params.arguments = database.lookup(src_path, options).arguments;

    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            params.add_remapped_file(file, source.content);
        } else {
            std::string path = path::is_absolute(file) ? file.str() : path::join(".", file);
            params.add_remapped_file(path, source.content);
        }
    }
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
    auto command = std::format("clang++ {} {} -fms-extensions", standard, src_path);

    database.add_command("fake", src_path, command);
    params.kind = CompilationKind::Preamble;

    CommandOptions options;
    options.query_toolchain = true;
    options.suppress_logging = true;

    params.arguments = database.lookup(src_path, options).arguments;

    auto pch_path = fs::createTemporaryFile("clice", "pch");
    if(!pch_path) {
        LOG_ERROR("{}", pch_path.error().message());
        return false;
    }

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

void Tester::clear() {
    params = CompilationParams();
    database = CompilationDatabase();
    unit.reset();
    sources.all_files.clear();
    src_path.clear();
}

}  // namespace clice::testing
