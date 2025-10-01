#pragma once

#include "Test.h"
#include "Annotation.h"
#include "Protocol/Protocol.h"
#include "Compiler/Command.h"
#include "Compiler/Compilation.h"

namespace clice::testing {

struct Tester {
    CompilationParams params;
    CompilationDatabase database;
    std::optional<CompilationUnit> unit;
    std::string src_path;

    /// All sources file in the compilation.
    AnnotatedSources sources;

    void add_main(llvm::StringRef file, llvm::StringRef content) {
        src_path = file;
        sources.add_source(file, content);
    }

    void add_file(llvm::StringRef name, llvm::StringRef content) {
        sources.add_source(name, content);
    }

    void add_files(llvm::StringRef main_file, llvm::StringRef content) {
        src_path = main_file;
        sources.add_sources(content);
    }

    void prepare(llvm::StringRef standard = "-std=c++20") {
        auto command = std::format("clang++ {} {} -fms-extensions", standard, src_path);

        database.update_command("fake", src_path, command);
        params.kind = CompilationUnit::Content;

        CommandOptions options;
        options.resource_dir = true;
        options.query_driver = true;
        options.suppress_logging = true;
        params.arguments = database.get_command(src_path, options).arguments;

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

    bool compile(llvm::StringRef standard = "-std=c++20") {
        prepare(standard);

        auto info = clice::compile(params);
        if(!info) {
            return false;
        }

        this->unit.emplace(std::move(*info));
        return true;
    }

    bool compile_with_pch(llvm::StringRef standard = "-std=c++20") {
        params.diagnostics = std::make_shared<std::vector<Diagnostic>>();
        auto command = std::format("clang++ {} {} -fms-extensions", standard, src_path);

        database.update_command("fake", src_path, command);
        params.kind = CompilationUnit::Preamble;

        CommandOptions options;
        options.resource_dir = true;
        options.query_driver = true;
        options.suppress_logging = true;
        params.arguments = database.get_command(src_path, options).arguments;

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
                llvm::outs() << unit.error() << "\n";
                for(auto& diag: *params.diagnostics) {
                    std::println("{}", diag.message);
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
            return false;
        }

        this->unit.emplace(std::move(*unit));
        return true;
    }

    std::uint32_t operator[] (llvm::StringRef file, llvm::StringRef pos) {
        return sources.all_files.lookup(file).offsets.lookup(pos);
    }

    std::uint32_t point(llvm::StringRef name = "", llvm::StringRef file = "") {
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

    llvm::ArrayRef<std::uint32_t> nameless_points(llvm::StringRef file = "") {
        if(file.empty()) {
            file = src_path;
        }

        return sources.all_files[file].nameless_offsets;
    }

    LocalSourceRange range(llvm::StringRef name = "", llvm::StringRef file = "") {
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

    void clear() {
        params = CompilationParams();
        database = CompilationDatabase();
        unit.reset();
        sources.all_files.clear();
        src_path.clear();
    }
};

}  // namespace clice::testing
