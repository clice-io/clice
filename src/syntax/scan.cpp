#include "syntax/scan.h"

#include "syntax/lexer.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CompilationDatabase.h"

namespace clice {

ScanResult scan(llvm::StringRef content) {
    ScanResult result;

    Lexer lexer(content, true, nullptr, false);

    int conditional_depth = 0;

    while(true) {
        auto token = lexer.advance();
        if(token.is_eof()) {
            break;
        }

        if(!token.is_at_start_of_line) {
            continue;
        }

        if(token.kind == clang::tok::hash) {
            auto directive = lexer.advance();
            if(directive.is_eof()) {
                break;
            }

            auto spelling = directive.text(content);

            if(spelling == "if" || spelling == "ifdef" || spelling == "ifndef") {
                conditional_depth++;
                lexer.advance_until(clang::tok::eod);
            } else if(spelling == "endif") {
                if(conditional_depth > 0) {
                    conditional_depth--;
                }
                lexer.advance_until(clang::tok::eod);
            } else if(spelling == "elif" || spelling == "elifdef" || spelling == "elifndef" ||
                      spelling == "else") {
                lexer.advance_until(clang::tok::eod);
            } else if(spelling == "include") {
                auto header = lexer.advance();
                if(header.is_header_name()) {
                    auto name = header.text(content);
                    // Strip <> or "" delimiters
                    result.includes.emplace_back(name.substr(1, name.size() - 2));
                }
                lexer.advance_until(clang::tok::eod);
            } else {
                lexer.advance_until(clang::tok::eod);
            }
        } else if(token.is_identifier()) {
            auto spelling = token.text(content);
            bool is_export = false;

            if(spelling == "export") {
                is_export = true;
                auto next = lexer.advance();
                if(next.is_eof()) {
                    break;
                }
                if(!next.is_identifier() || next.text(content) != "module") {
                    continue;
                }
                spelling = "module";
            }

            if(spelling == "module") {
                auto next = lexer.next();
                if(next.is_eof()) {
                    break;
                }

                // `module;` is global module fragment, skip it
                if(next.kind == clang::tok::semi) {
                    lexer.advance();
                    continue;
                }

                // Module declaration inside conditional directive
                if(conditional_depth > 0) {
                    result.need_preprocess = true;
                    return result;
                }

                // Collect module name: identifiers, '.', ':'
                std::string module_name;
                while(true) {
                    auto tok = lexer.advance();
                    if(tok.is_eof() || tok.kind == clang::tok::semi || tok.is_eod()) {
                        break;
                    }
                    if(tok.is_identifier()) {
                        module_name += tok.text(content);
                    } else if(tok.kind == clang::tok::period) {
                        module_name += '.';
                    } else if(tok.kind == clang::tok::colon) {
                        module_name += ':';
                    }
                }

                result.module_name = std::move(module_name);
                result.is_interface_unit = is_export;
            }
        }
    }

    return result;
}

namespace {

class InMemoryFile : public llvm::vfs::File {
public:
    explicit InMemoryFile(std::unique_ptr<llvm::MemoryBuffer> buffer, llvm::vfs::Status status) :
        buffer(std::move(buffer)), file_status(std::move(status)) {}

    llvm::ErrorOr<llvm::vfs::Status> status() override {
        return file_status;
    }

    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
        getBuffer(const llvm::Twine&, int64_t, bool, bool) override {
        return llvm::MemoryBuffer::getMemBufferCopy(buffer->getBuffer(),
                                                    buffer->getBufferIdentifier());
    }

    std::error_code close() override {
        return {};
    }

private:
    std::unique_ptr<llvm::MemoryBuffer> buffer;
    llvm::vfs::Status file_status;
};

/// Strip file content to only #include lines for fast preprocessing.
std::string strip_to_includes(llvm::StringRef content) {
    std::string result;

    Lexer lexer(content, true, nullptr, false);

    while(true) {
        auto token = lexer.advance();
        if(token.is_eof()) {
            break;
        }

        if(token.is_at_start_of_line && token.kind == clang::tok::hash) {
            auto directive = lexer.advance();
            if(directive.is_eof()) {
                break;
            }

            auto spelling = directive.text(content);
            if(spelling == "include") {
                // Keep the whole directive line
                auto start = token.range.begin;
                auto eod = lexer.advance_until(clang::tok::eod);
                auto end = eod.range.begin;
                result += content.substr(start, end - start);
                result += '\n';
            } else {
                lexer.advance_until(clang::tok::eod);
            }
        }
    }

    return result;
}

class IncludeOnlyVFS : public llvm::vfs::ProxyFileSystem {
public:
    explicit IncludeOnlyVFS(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> base) :
        ProxyFileSystem(std::move(base)) {}

    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
        openFileForRead(const llvm::Twine& path) override {
        llvm::SmallString<256> storage;
        llvm::StringRef path_str = path.toStringRef(storage);

        // If already visited, return empty buffer to cut include chains
        if(!visited.insert(path_str).second) {
            auto status_or = getUnderlyingFS().status(path);
            if(!status_or) {
                return status_or.getError();
            }
            auto empty = llvm::MemoryBuffer::getMemBuffer("", path_str);
            return std::make_unique<InMemoryFile>(std::move(empty), *status_or);
        }

        // First visit: read real file, strip to includes only
        auto file = getUnderlyingFS().openFileForRead(path);
        if(!file) {
            return file;
        }

        auto status_or = (*file)->status();
        if(!status_or) {
            return status_or.getError();
        }

        auto buffer = (*file)->getBuffer(path_str, -1, true, false);
        if(!buffer) {
            return buffer.getError();
        }

        auto stripped = strip_to_includes((*buffer)->getBuffer());
        auto new_buffer = llvm::MemoryBuffer::getMemBufferCopy(stripped, path_str);
        return std::make_unique<InMemoryFile>(std::move(new_buffer), *status_or);
    }

private:
    llvm::StringSet<> visited;
};

class ScanPPCallbacks : public clang::PPCallbacks {
public:
    explicit ScanPPCallbacks(ScanResult& result) : result(result) {}

    void InclusionDirective(clang::SourceLocation,
                            const clang::Token&,
                            llvm::StringRef,
                            bool,
                            clang::CharSourceRange,
                            clang::OptionalFileEntryRef file,
                            llvm::StringRef,
                            llvm::StringRef,
                            const clang::Module*,
                            bool,
                            clang::SrcMgr::CharacteristicKind) override {
        if(file) {
            result.includes.emplace_back(file->getFileEntry().tryGetRealPathName().str());
        }
    }

    void moduleImport(clang::SourceLocation,
                      clang::ModuleIdPath names,
                      const clang::Module*) override {
        std::string name;
        for(auto& part: names) {
            if(!name.empty()) {
                name += '.';
            }
            name += part.getIdentifierInfo()->getName();
        }
        result.modules.emplace_back(std::move(name));
    }

private:
    ScanResult& result;
};

}  // namespace

ScanResult scan_with_preprocessor(llvm::ArrayRef<const char*> arguments,
                                  llvm::StringRef directory,
                                  bool arguments_from_database,
                                  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs) {
    ScanResult result;

    if(!vfs) {
        vfs = llvm::vfs::createPhysicalFileSystem();
    }

    auto hooked_vfs = llvm::makeIntrusiveRefCnt<IncludeOnlyVFS>(std::move(vfs));

    // Create compiler invocation
    clang::DiagnosticOptions diag_opts;
    auto diag_engine = clang::CompilerInstance::createDiagnostics(*hooked_vfs,
                                                                  diag_opts,
                                                                  new clang::IgnoringDiagConsumer(),
                                                                  true);

    std::unique_ptr<clang::CompilerInvocation> invocation;

    if(arguments_from_database) {
        invocation = std::make_unique<clang::CompilerInvocation>();
        if(!clang::CompilerInvocation::CreateFromArgs(*invocation,
                                                      llvm::ArrayRef(arguments).drop_front(),
                                                      *diag_engine,
                                                      arguments[0])) {
            return result;
        }
    } else {
        clang::CreateInvocationOptions options = {
            .Diags = diag_engine,
            .VFS = hooked_vfs,
            .ProbePrecompiled = false,
        };
        invocation = clang::createInvocation(arguments, options);
        if(!invocation) {
            return result;
        }
    }

    invocation->getFrontendOpts().DisableFree = false;

    // Set working directory
    invocation->getFileSystemOpts().WorkingDir = directory.str();

    auto instance = std::make_unique<clang::CompilerInstance>(std::move(invocation));
    instance->createDiagnostics(*hooked_vfs, new clang::IgnoringDiagConsumer(), true);
    instance->createFileManager(hooked_vfs);

    if(!instance->createTarget()) {
        return result;
    }

    auto action = std::make_unique<clang::PreprocessOnlyAction>();

    if(!action->BeginSourceFile(*instance, instance->getFrontendOpts().Inputs[0])) {
        return result;
    }

    instance->getPreprocessor().addPPCallbacks(std::make_unique<ScanPPCallbacks>(result));

    if(auto error = action->Execute()) {
        llvm::consumeError(std::move(error));
    }

    action->EndSourceFile();

    // Get module name from preprocessor
    auto& pp = instance->getPreprocessor();
    auto module_name = pp.getNamedModuleName();
    if(!module_name.empty()) {
        result.module_name = module_name;
        result.is_interface_unit = pp.isInNamedInterfaceUnit();
    }

    return result;
}

std::uint32_t compute_preamble_bound(llvm::StringRef content) {
    auto result = compute_preamble_bounds(content);
    if(result.empty()) {
        return 0;
    } else {
        return result.back();
    }
}

std::vector<std::uint32_t> compute_preamble_bounds(llvm::StringRef content) {
    std::vector<std::uint32_t> result;

    Lexer lexer(content, true, nullptr, false);

    while(true) {
        auto token = lexer.advance();
        if(token.is_eof()) {
            break;
        }

        if(token.is_at_start_of_line) {
            if(token.kind == clang::tok::hash) {
                /// For preprocessor directive, consume the whole directive.
                lexer.advance_until(clang::tok::eod);
                auto last = lexer.last();

                /// Append the token before the eod.
                result.push_back(last.range.end);
            } else if(token.is_identifier() && token.text(content) == "module") {
                /// If we encounter a module keyword at the start of a line, it may be
                /// a module declaration or global module fragment.
                auto next = lexer.next();

                if(next.kind == clang::tok::semi) {
                    /// If next token is `;`, it is a global module fragment.
                    /// we just continue.
                    lexer.advance();

                    /// Append it to bounds.
                    result.push_back(next.range.end);
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }

    return result;
}

}  // namespace clice
