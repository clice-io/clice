#include "syntax/scan.h"

#include <deque>

#include "syntax/lexer.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Tooling/CompilationDatabase.h"

namespace clice {

ScanResult scan(llvm::StringRef content) {
    namespace dds = clang::dependency_directives_scan;

    ScanResult result;

    llvm::SmallVector<dds::Token> tokens;
    llvm::SmallVector<dds::Directive> directives;

    if(clang::scanSourceForDependencyDirectives(content, tokens, directives)) {
        return result;
    }

    // Most source files have 10-30 includes; pre-allocate to avoid reallocs.
    result.includes.reserve(std::min<std::size_t>(directives.size(), 32));

    int conditional_depth = 0;

    for(auto& dir: directives) {
        switch(dir.Kind) {
            case dds::pp_if:
            case dds::pp_ifdef:
            case dds::pp_ifndef: {
                conditional_depth++;
                break;
            }
            case dds::pp_endif: {
                if(conditional_depth > 0) {
                    conditional_depth--;
                }
                break;
            }
            case dds::pp_elif:
            case dds::pp_elifdef:
            case dds::pp_elifndef:
            case dds::pp_else: {
                break;
            }
            case dds::pp_include:
            case dds::pp_include_next:
            case dds::pp___include_macros: {
                // Find the header token (string_literal or header_name).
                for(auto& tok: dir.Tokens) {
                    if(tok.is(clang::tok::header_name) || tok.is(clang::tok::string_literal)) {
                        auto name = content.substr(tok.Offset, tok.Length);
                        // Strip <> or "" delimiters.
                        if(name.size() >= 2) {
                            bool angled = name.front() == '<';
                            ScanResult::IncludeInfo info;
                            info.path = std::string(name.substr(1, name.size() - 2));
                            info.conditional = conditional_depth > 0;
                            info.is_angled = angled;
                            info.is_include_next = dir.Kind == dds::pp_include_next;
                            result.includes.push_back(std::move(info));
                        }
                        break;
                    }
                }
                break;
            }
            case dds::cxx_module_decl:
            case dds::cxx_export_module_decl: {
                if(conditional_depth > 0) {
                    result.need_preprocess = true;
                    return result;
                }

                // Collect module name from tokens: skip keywords, then
                // collect identifiers, '.', ':'.
                std::string module_name;
                bool seen_module_keyword = false;
                for(auto& tok: dir.Tokens) {
                    if(!seen_module_keyword) {
                        if(tok.is(clang::tok::raw_identifier)) {
                            auto spelling = content.substr(tok.Offset, tok.Length);
                            if(spelling == "module") {
                                seen_module_keyword = true;
                            }
                        }
                        continue;
                    }
                    if(tok.is(clang::tok::raw_identifier)) {
                        module_name += content.substr(tok.Offset, tok.Length);
                    } else if(tok.is(clang::tok::period)) {
                        module_name += '.';
                    } else if(tok.is(clang::tok::colon)) {
                        module_name += ':';
                    }
                }

                result.module_name = std::move(module_name);
                result.is_interface_unit = (dir.Kind == dds::cxx_export_module_decl);
                break;
            }
            default: {
                break;
            }
        }
    }

    return result;
}

namespace {

class ScanDirectivesGetter : public clang::DependencyDirectivesGetter {
public:
    ScanDirectivesGetter(SharedScanCache* cache, clang::FileManager& file_mgr) :
        cache(cache), file_mgr(&file_mgr) {}

    std::unique_ptr<clang::DependencyDirectivesGetter>
        cloneFor(clang::FileManager& new_file_mgr) override {
        return std::make_unique<ScanDirectivesGetter>(cache, new_file_mgr);
    }

    std::optional<llvm::ArrayRef<clang::dependency_directives_scan::Directive>>
        operator()(clang::FileEntryRef file) override {
        auto path = file.getFileEntry().tryGetRealPathName();
        if(path.empty()) {
            path = file.getName();
        }

        // Check cache first.
        if(cache) {
            auto it = cache->entries.find(path);
            if(it != cache->entries.end()) {
                return llvm::ArrayRef(it->second.directives);
            }
        }

        // Read the file content.
        auto buffer = file_mgr->getBufferForFile(file);
        if(!buffer) {
            return std::nullopt;
        }

        auto source = (*buffer)->getBuffer().str();

        // Create entry in its final location first, then scan into it.
        // Directive::Tokens are ArrayRefs pointing into the tokens SmallVector,
        // so the entry must not be moved after scanning.
        SharedScanCache::CachedEntry* entry_ptr;
        if(cache) {
            auto [it, _] = cache->entries.try_emplace(path);
            entry_ptr = &it->second;
        } else {
            local_entries.emplace_back();
            entry_ptr = &local_entries.back();
        }

        entry_ptr->source = std::move(source);

        if(clang::scanSourceForDependencyDirectives(entry_ptr->source,
                                                    entry_ptr->tokens,
                                                    entry_ptr->directives)) {
            // Scan failed — remove the entry.
            if(cache) {
                cache->entries.erase(path);
            } else {
                local_entries.pop_back();
            }
            return std::nullopt;
        }

        return llvm::ArrayRef(entry_ptr->directives);
    }

private:
    SharedScanCache* cache;
    clang::FileManager* file_mgr;
    std::deque<SharedScanCache::CachedEntry> local_entries;
};

/// PPCallbacks for precise mode: single ScanResult with accurate
/// conditional tracking via preprocessor callbacks.
class PreciseScanPPCallbacks : public clang::PPCallbacks {
public:
    explicit PreciseScanPPCallbacks(ScanResult& result) : result(result) {}

    void InclusionDirective(clang::SourceLocation,
                            const clang::Token& include_tok,
                            llvm::StringRef file_name,
                            bool is_angled,
                            clang::CharSourceRange,
                            clang::OptionalFileEntryRef file,
                            llvm::StringRef,
                            llvm::StringRef,
                            const clang::Module*,
                            bool,
                            clang::SrcMgr::CharacteristicKind) override {
        bool not_found = !file.has_value();
        std::string resolved_path;
        if(file) {
            resolved_path = file->getFileEntry().tryGetRealPathName().str();
        } else {
            resolved_path = file_name.str();
        }

        ScanResult::IncludeInfo info;
        info.path = std::move(resolved_path);
        info.conditional = conditional_depth > 0;
        info.not_found = not_found;
        info.is_angled = is_angled;
        info.is_include_next =
            include_tok.getIdentifierInfo() &&
            include_tok.getIdentifierInfo()->getPPKeywordID() == clang::tok::pp_include_next;
        result.includes.push_back(std::move(info));
    }

    void If(clang::SourceLocation, clang::SourceRange, ConditionValueKind) override {
        conditional_depth++;
    }

    void Ifdef(clang::SourceLocation, const clang::Token&, const clang::MacroDefinition&) override {
        conditional_depth++;
    }

    void Ifndef(clang::SourceLocation,
                const clang::Token&,
                const clang::MacroDefinition&) override {
        conditional_depth++;
    }

    void Endif(clang::SourceLocation, clang::SourceLocation) override {
        if(conditional_depth > 0) {
            conditional_depth--;
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
    int conditional_depth = 0;
};

/// Create and configure a CompilerInstance for scanning.
/// If content is non-empty, it is used as remapped source for the main file.
std::unique_ptr<clang::CompilerInstance>
    create_scan_instance(llvm::ArrayRef<const char*> arguments,
                         llvm::StringRef directory,
                         llvm::StringRef content,
                         llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs) {
    clang::DiagnosticOptions diag_opts;
    auto diag_engine = clang::CompilerInstance::createDiagnostics(*vfs,
                                                                  diag_opts,
                                                                  new clang::IgnoringDiagConsumer(),
                                                                  true);

    std::unique_ptr<clang::CompilerInvocation> invocation;

    bool is_cc1 = arguments.size() >= 2 && llvm::StringRef(arguments[1]) == "-cc1";
    if(is_cc1) {
        invocation = std::make_unique<clang::CompilerInvocation>();
        if(!clang::CompilerInvocation::CreateFromArgs(*invocation,
                                                      llvm::ArrayRef(arguments).drop_front(2),
                                                      *diag_engine,
                                                      arguments[0])) {
            return nullptr;
        }
    } else {
        clang::CreateInvocationOptions options = {
            .Diags = diag_engine,
            .VFS = vfs,
            .ProbePrecompiled = false,
        };
        invocation = clang::createInvocation(arguments, options);
        if(!invocation) {
            return nullptr;
        }
    }

    invocation->getFrontendOpts().DisableFree = false;
    invocation->getFileSystemOpts().WorkingDir = directory.str();

    if(!content.empty()) {
        auto& inputs = invocation->getFrontendOpts().Inputs;
        if(!inputs.empty()) {
            auto main_file = inputs[0].getFile();
            // Use an overlay VFS to inject the remapped content. This ensures
            // both the preprocessor and the DependencyDirectivesGetter see it.
            auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(vfs);
            auto mem_fs = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
            mem_fs->addFile(main_file, 0, llvm::MemoryBuffer::getMemBufferCopy(content, main_file));
            overlay->pushOverlay(std::move(mem_fs));
            vfs = std::move(overlay);
        }
    }

    auto instance = std::make_unique<clang::CompilerInstance>(std::move(invocation));
    instance->createDiagnostics(*vfs, new clang::IgnoringDiagConsumer(), true);
    instance->getDiagnostics().setSuppressAllDiagnostics(true);
    instance->createFileManager(vfs);

    return instance;
}

}  // namespace

ScanResult scan_precise(llvm::ArrayRef<const char*> arguments,
                        llvm::StringRef directory,
                        llvm::StringRef content,
                        SharedScanCache* cache,
                        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs) {
    ScanResult result;

    if(!vfs) {
        vfs = llvm::vfs::createPhysicalFileSystem();
    }

    auto instance = create_scan_instance(arguments, directory, content, vfs);
    if(!instance) {
        return result;
    }

    auto getter = std::make_unique<ScanDirectivesGetter>(cache, instance->getFileManager());
    instance->setDependencyDirectivesGetter(std::move(getter));

    if(!instance->createTarget()) {
        return result;
    }

    auto action = std::make_unique<clang::PreprocessOnlyAction>();

    if(!action->BeginSourceFile(*instance, instance->getFrontendOpts().Inputs[0])) {
        return result;
    }

    instance->getPreprocessor().addPPCallbacks(std::make_unique<PreciseScanPPCallbacks>(result));

    if(auto error = action->Execute()) {
        llvm::consumeError(std::move(error));
    }

    action->EndSourceFile();

    // Get module name from preprocessor.
    auto& pp = instance->getPreprocessor();
    if(pp.isInNamedModule()) {
        result.module_name = pp.getNamedModuleName();
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
