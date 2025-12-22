#include "Test/Tester.h"

namespace clice::testing {

void Tester::prepare(llvm::StringRef standard) {
    auto command = std::format("clang++ {} {} -fms-extensions", standard, src_path);

    database.add_command("fake", src_path, command);
    params.kind = CompilationUnit::Content;

    CommandOptions options;
    options.resource_dir = true;
    options.query_toolchain = true;
    options.suppress_logging = true;

    params.arguments_from_database = true;
    params.arguments = database.lookup(src_path, options).arguments;

    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            params.add_remapped_file(file, source.content);
        } else {
            /// FIXME: This is a workaround.
            std::string path = path::is_absolute(file) ? file.str() : path::join(".", file);
            params.add_remapped_file(path, source.content);
        }
    }
}

bool Tester::compile(llvm::StringRef standard) {
    prepare(standard);

    auto unit = clice::compile(params);
    if(!unit) {
        LOG_ERROR("{}", unit.error());
        for(auto& diag: *params.diagnostics) {
            LOG_ERROR("{}", diag.message);
        }
        return false;
    }

    this->unit.emplace(std::move(*unit));
    return true;
}

bool Tester::compile_with_pch(llvm::StringRef standard) {
    params.diagnostics = std::make_shared<std::vector<Diagnostic>>();
    auto command = std::format("clang++ {} {} -fms-extensions", standard, src_path);

    database.add_command("fake", src_path, command);
    params.kind = CompilationUnit::Preamble;

    CommandOptions options;
    options.resource_dir = true;
    options.query_toolchain = true;
    options.suppress_logging = true;

    params.arguments_from_database = true;
    params.arguments = database.lookup(src_path, options).arguments;

    auto path = fs::createTemporaryFile("clice", "pch");
    if(!path) {
        llvm::outs() << path.error().message() << "\n";
    }

    /// Build PCH
    params.output_file = *path;

    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            auto bound = compute_preamble_bound(source.content);
            params.add_remapped_file(file, source.content, bound);
        } else {
            /// FIXME: This is a workaround.
            std::string path = path::is_absolute(file) ? file.str() : path::join(".", file);
            params.add_remapped_file(path, source.content);
        }
    }

    PCHInfo info;
    {
        auto unit = clice::compile(params, info);
        if(!unit) {
            LOG_ERROR("{}", unit.error());
            for(auto& diag: *params.diagnostics) {
                LOG_ERROR("{}", diag.message);
            }
            return false;
        }
    }

    /// Build AST
    params.output_file.clear();
    params.kind = CompilationUnit::Content;
    params.pch = {info.path, info.preamble.size()};
    for(auto& [file, source]: sources.all_files) {
        if(file == src_path) {
            params.add_remapped_file(file, source.content);
        } else {
            /// FIXME: This is a workaround.
            std::string path = path::is_absolute(file) ? file.str() : path::join(".", file);
            params.add_remapped_file(path, source.content);
        }
    }

    auto unit = clice::compile(params);
    if(!unit) {
        LOG_ERROR("{}", unit.error());
        for(auto& diag: *params.diagnostics) {
            LOG_ERROR("{}", diag.message);
        }
        return false;
    }

    this->unit.emplace(std::move(*unit));
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
    } else {
        assert(offsets.contains(name));
        return offsets.lookup(name);
    }
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
    } else {
        assert(ranges.contains(name));
        return ranges.lookup(name);
    }
}

void Tester::clear() {
    params = CompilationParams();
    database = CompilationDatabase();
    unit.reset();
    sources.all_files.clear();
    src_path.clear();
}

}  // namespace clice::testing
