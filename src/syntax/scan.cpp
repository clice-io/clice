module clice;

import :command.invocation;
import :syntax.lexer;
import :syntax.scan;
import :vfs.file_system;

namespace clice {

/// Whether a line of the text starts with `module` or `export module`.
static bool declares_module(llvm::StringRef content) {
    Lexer lexer(content, dialect_of());
    while(true) {
        auto token = lexer.advance();
        if(token.is_eof()) {
            return false;
        }
        if(!token.is_at_start_of_line || !token.is_identifier()) {
            continue;
        }
        if(token.text(content) == "export") {
            token = lexer.advance();
        }
        if(token.is_identifier() && token.text(content) == "module") {
            return true;
        }
    }
}

ScanResult scan_quick(llvm::StringRef content) {
    namespace dds = clang::dependency_directives_scan;

    ScanResult result;

    llvm::SmallVector<dds::Token> tokens;
    llvm::SmallVector<dds::Directive> directives;

    if(clang::scanSourceForDependencyDirectives(content, tokens, directives)) {
        // A precise scan lexes text the directive scanner rejects in full,
        // and the preprocessor reads a module declaration in it — one the
        // scanner balks at, as with its `;` on the next line, included.
        result.directives_hash = llvm::xxh3_64bits(content);
        result.need_preprocess = declares_module(content);
        return result;
    }

    // Most source files have 10-30 includes; pre-allocate to avoid reallocs.
    result.includes.reserve(std::min<std::size_t>(directives.size(), 32));

    int conditional_depth = 0;
    llvm::SmallString<1024> stream;

    for(auto& dir: directives) {
        stream.push_back(static_cast<char>(dir.Kind));
        for(auto& tok: dir.Tokens) {
            // The flags tell `F(x)` from `F (x)` in a #define.
            stream += content.substr(tok.Offset, tok.Length);
            stream.append(reinterpret_cast<const char*>(&tok.Flags),
                          reinterpret_cast<const char*>(&tok.Flags) + sizeof(tok.Flags));
        }
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
                            info.offset = dir.Tokens.front().Offset;
                            info.name_offset = tok.Offset;
                            info.name_length = tok.Length;
                            info.conditional = conditional_depth > 0;
                            info.conditional_depth = static_cast<std::uint16_t>(conditional_depth);
                            info.is_angled = angled;
                            info.is_include_next = dir.Kind == dds::pp_include_next;
                            result.includes.push_back(std::move(info));
                        }
                        break;
                    }
                }
                break;
            }
            case dds::cxx_import_decl:
            case dds::cxx_export_import_decl: {
                result.has_import = true;
                break;
            }
            case dds::cxx_module_decl:
            case dds::cxx_export_module_decl: {
                if(conditional_depth > 0) {
                    // The name needs scan_module_decl(); keep scanning —
                    // includes and import detection past this point are
                    // still lexical truth.
                    result.need_preprocess = true;
                    break;
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

                result.is_interface_unit = dir.Kind == dds::cxx_export_module_decl;
                result.module_name = std::move(module_name);
                break;
            }
            default: {
                break;
            }
        }
    }

    result.directives_hash = llvm::xxh3_64bits(stream);
    return result;
}

namespace {

class DirectivesGetter : public clang::DependencyDirectivesGetter {
public:
    explicit DirectivesGetter(clang::FileManager& file_mgr) : file_mgr(&file_mgr) {}

    std::unique_ptr<clang::DependencyDirectivesGetter>
        cloneFor(clang::FileManager& new_file_mgr) override {
        return std::make_unique<DirectivesGetter>(new_file_mgr);
    }

    std::optional<llvm::ArrayRef<clang::dependency_directives_scan::Directive>>
        operator()(clang::FileEntryRef file) override {
        if(file == scanned) {
            return std::nullopt;
        }

        auto buffer = file_mgr->getBufferForFile(file);
        if(!buffer) {
            return std::nullopt;
        }

        // Directive::Tokens are ArrayRefs into the entry's tokens, so the
        // entry is scanned in its final, never-moving place.
        auto& entry = entries.emplace_back();
        entry.source = (*buffer)->getBuffer().str();
        if(clang::scanSourceForDependencyDirectives(entry.source, entry.tokens, entry.directives)) {
            entries.pop_back();
            return std::nullopt;
        }
        return llvm::ArrayRef(entry.directives);
    }

    /// Lexed by clang's ordinary lexer, not from its directives: the scanned
    /// file is the text being typed, and the directives lexer breaks on
    /// half-typed directives (an `#if(` cut off at the end of the buffer,
    /// `import <header>;`) where the ordinary lexer reports an error — the
    /// scan runs in the master process, where a crash ends the server.
    clang::OptionalFileEntryRef scanned;

private:
    struct Entry {
        /// The file's text, which the tokens point into.
        std::string source;
        llvm::SmallVector<clang::dependency_directives_scan::Token> tokens;
        llvm::SmallVector<clang::dependency_directives_scan::Directive> directives;
    };

    clang::FileManager* file_mgr;
    std::deque<Entry> entries;
};

/// Collects the imports the preprocessor reaches, each with its offset in
/// the scanned file.
class ImportRecorder : public clang::PPCallbacks {
public:
    ImportRecorder(ScanResult& result, const clang::SourceManager& sources) :
        result(result), sources(sources) {}

    void moduleImport(clang::SourceLocation location,
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

        std::uint32_t offset = 0;
        for(auto loc = sources.getExpansionLoc(location); loc.isValid();
            loc = sources.getIncludeLoc(sources.getFileID(loc))) {
            if(sources.getFileID(loc) == sources.getMainFileID()) {
                offset = sources.getFileOffset(loc);
                break;
            }
        }
        result.import_offsets.push_back(offset);
    }

private:
    ScanResult& result;
    const clang::SourceManager& sources;
};

/// The setup every preprocessor-driven scan shares: the instance from the
/// command (diagnostics ignored), the directives getter, the target, and
/// the scanned file entered through a preprocess-only action. `body` runs on
/// the entered preprocessor; the module declaration it reached is read
/// into `result` before the source file is ended.
void scan_with_preprocessor(
    llvm::ArrayRef<const char*> arguments,
    llvm::StringRef directory,
    std::optional<llvm::StringRef> content,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs,
    ScanResult& result,
    llvm::function_ref<void(clang::CompilerInstance&, clang::FrontendAction&)> body) {
    if(!vfs) {
        vfs = new vfs::View();
    }

    clang::DiagnosticOptions diag_opts;
    auto diag_engine = clang::CompilerInstance::createDiagnostics(*vfs,
                                                                  diag_opts,
                                                                  new clang::IgnoringDiagConsumer(),
                                                                  true);
    auto invocation = create_compiler_invocation(arguments, directory, vfs, diag_engine);
    if(!invocation) {
        return;
    }

    // An engaged content remaps the scanned file to it, even when empty,
    // through an overlay VFS.
    if(content.has_value()) {
        auto& inputs = invocation->getFrontendOpts().Inputs;
        if(!inputs.empty()) {
            auto input = inputs[0].getFile();
            auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(vfs);
            auto mem_fs = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
            mem_fs->addFile(input, 0, llvm::MemoryBuffer::getMemBufferCopy(*content, input));
            overlay->pushOverlay(std::move(mem_fs));
            vfs = std::move(overlay);
        }
    }

    auto instance = std::make_unique<clang::CompilerInstance>(std::move(invocation));
    instance->setVirtualFileSystem(std::move(vfs));
    instance->createDiagnostics(new clang::IgnoringDiagConsumer(), true);
    instance->getDiagnostics().setSuppressAllDiagnostics(true);
    instance->createFileManager();

    auto getter = std::make_unique<DirectivesGetter>(instance->getFileManager());
    auto& directives = *getter;
    instance->setDependencyDirectivesGetter(std::move(getter));

    if(!instance->createTarget()) {
        return;
    }

    auto action = std::make_unique<clang::PreprocessOnlyAction>();
    if(!action->BeginSourceFile(*instance, instance->getFrontendOpts().Inputs[0])) {
        return;
    }
    auto& sources = instance->getSourceManager();
    directives.scanned = sources.getFileEntryRefForID(sources.getMainFileID());

    body(*instance, *action);

    auto& pp = instance->getPreprocessor();
    if(pp.isInNamedModule()) {
        result.module_name = pp.getNamedModuleName();
        result.is_interface_unit = pp.isInNamedInterfaceUnit();
    }

    action->EndSourceFile();
}

}  // namespace

std::shared_ptr<const ScanResult> QuickScanCache::scan_of(Fid fid,
                                                          std::uint64_t content_hash,
                                                          llvm::StringRef content) {
    auto [it, inserted] = results.try_emplace({fid, content_hash});
    if(inserted) {
        it->second = std::make_shared<const ScanResult>(scan_quick(content));
    }
    return it->second;
}

ScanResult scan_precise(llvm::ArrayRef<const char*> arguments,
                        llvm::StringRef directory,
                        std::optional<llvm::StringRef> content,
                        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs) {
    ScanResult result;
    scan_with_preprocessor(
        arguments,
        directory,
        content,
        std::move(vfs),
        result,
        [&](clang::CompilerInstance& instance, clang::FrontendAction& action) {
            instance.getPreprocessor().addPPCallbacks(
                std::make_unique<ImportRecorder>(result, instance.getSourceManager()));
            if(auto error = action.Execute()) {
                llvm::consumeError(std::move(error));
            }
        });
    return result;
}

ScanResult scan_module_decl(llvm::ArrayRef<const char*> arguments,
                            llvm::StringRef directory,
                            std::optional<llvm::StringRef> content,
                            llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs) {
    ScanResult result;
    scan_with_preprocessor(arguments,
                           directory,
                           content,
                           std::move(vfs),
                           result,
                           [](clang::CompilerInstance& instance, clang::FrontendAction&) {
                               // Instead of Execute(), which processes the
                               // entire file, lex and stop as soon as the
                               // module declaration is found.
                               auto& pp = instance.getPreprocessor();
                               pp.EnterMainSourceFile();
                               clang::Token tok;
                               do {
                                   pp.Lex(tok);
                               } while(!pp.isInNamedModule() && tok.isNot(clang::tok::eof));
                           });
    return result;
}

}  // namespace clice
