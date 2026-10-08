module;

#include "modules/prelude.h"

module clice;

import :command.invocation;
import :syntax.lexer;
import :syntax.scan;
import :vfs.file_system;

namespace clice {

ScanResult scan_quick(llvm::StringRef content) {
    namespace dds = clang::dependency_directives_scan;

    ScanResult result;

    llvm::SmallVector<dds::Token> tokens;
    llvm::SmallVector<dds::Directive> directives;

    if(clang::scanSourceForDependencyDirectives(content, tokens, directives)) {
        // A precise scan lexes text the directive scanner rejects in full.
        result.directives_hash = llvm::xxh3_64bits(content);
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

                result.is_implementation_unit =
                    dir.Kind == dds::cxx_module_decl && !module_name.contains(':');
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
        if(file == main_file) {
            return std::nullopt;
        }
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

    /// Lexed by clang's ordinary lexer, not from its directives: the main
    /// file is the text being typed, and the directives lexer breaks on
    /// half-typed directives (an `#if(` cut off at the end of the buffer,
    /// `import <header>;`) where the ordinary lexer reports an error — the
    /// scan runs in the master process, where a crash ends the server.
    clang::OptionalFileEntryRef main_file;

private:
    SharedScanCache* cache;
    clang::FileManager* file_mgr;
    std::deque<SharedScanCache::CachedEntry> local_entries;
};

/// PPCallbacks for precise mode: single ScanResult with accurate
/// conditional tracking via preprocessor callbacks.
class PreciseScanPPCallbacks : public clang::PPCallbacks {
public:
    PreciseScanPPCallbacks(ScanResult& result, const clang::SourceManager& sources) :
        result(result), sources(sources) {}

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
        for(location = sources.getExpansionLoc(location); location.isValid();) {
            auto fid = sources.getFileID(location);
            if(fid == sources.getMainFileID()) {
                offset = sources.getFileOffset(location);
                break;
            }
            location = sources.getIncludeLoc(fid);
        }
        result.import_offsets.push_back(offset);
    }

private:
    ScanResult& result;
    const clang::SourceManager& sources;
    int conditional_depth = 0;
};

/// The setup every preprocessor-driven scan shares: the instance from the
/// command (diagnostics ignored), the directives getter, the target, and
/// the main file entered through a preprocess-only action. `body` runs on
/// the entered preprocessor; the module declaration it reached is read
/// into `result` before the source file is ended.
void scan_with_preprocessor(
    llvm::ArrayRef<const char*> arguments,
    llvm::StringRef directory,
    std::optional<llvm::StringRef> content,
    SharedScanCache* cache,
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

    // An engaged content remaps the main file to it, even when empty,
    // through an overlay VFS.
    if(content.has_value()) {
        auto& inputs = invocation->getFrontendOpts().Inputs;
        if(!inputs.empty()) {
            auto main_file = inputs[0].getFile();
            auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(vfs);
            auto mem_fs = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
            mem_fs->addFile(main_file,
                            0,
                            llvm::MemoryBuffer::getMemBufferCopy(*content, main_file));
            overlay->pushOverlay(std::move(mem_fs));
            vfs = std::move(overlay);
        }
    }

    auto instance = std::make_unique<clang::CompilerInstance>(std::move(invocation));
    instance->setVirtualFileSystem(std::move(vfs));
    instance->createDiagnostics(new clang::IgnoringDiagConsumer(), true);
    instance->getDiagnostics().setSuppressAllDiagnostics(true);
    instance->createFileManager();

    auto getter = std::make_unique<ScanDirectivesGetter>(cache, instance->getFileManager());
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
    directives.main_file = sources.getFileEntryRefForID(sources.getMainFileID());

    body(*instance, *action);

    auto& pp = instance->getPreprocessor();
    if(pp.isInNamedModule()) {
        result.module_name = pp.getNamedModuleName();
        result.is_implementation_unit = pp.isInImplementationUnit();
    }

    action->EndSourceFile();
}

}  // namespace

const ScanResult& QuickScanCache::scan_of(Fid fid,
                                          std::uint64_t content_hash,
                                          llvm::StringRef content) {
    auto [it, inserted] = results.try_emplace({fid, content_hash});
    if(inserted) {
        it->second = scan_quick(content);
    }
    return it->second;
}

ScanResult scan_precise(llvm::ArrayRef<const char*> arguments,
                        llvm::StringRef directory,
                        std::optional<llvm::StringRef> content,
                        SharedScanCache* cache,
                        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs) {
    ScanResult result;
    scan_with_preprocessor(
        arguments,
        directory,
        content,
        cache,
        std::move(vfs),
        result,
        [&](clang::CompilerInstance& instance, clang::FrontendAction& action) {
            instance.getPreprocessor().addPPCallbacks(
                std::make_unique<PreciseScanPPCallbacks>(result, instance.getSourceManager()));
            if(auto error = action.Execute()) {
                llvm::consumeError(std::move(error));
            }
        });
    return result;
}

ScanResult scan_module_decl(llvm::ArrayRef<const char*> arguments,
                            llvm::StringRef directory,
                            std::optional<llvm::StringRef> content,
                            SharedScanCache* cache,
                            llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs) {
    ScanResult result;
    scan_with_preprocessor(arguments,
                           directory,
                           content,
                           cache,
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

    Lexer lexer(content);

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

bool is_preamble_complete(llvm::StringRef content, std::uint32_t bound) {
    // The full (NUL-terminated) content is lexed; `bound` applies as a
    // logical end — tokens starting at or past it belong to the body.
    Lexer lexer(content);

    // Tokens of the current logical line; eod tokens only act as line ends.
    llvm::SmallVector<Token, 8> line;

    auto line_complete = [&] {
        // A header-name directive (#include, #embed, ...) is complete once
        // it has a terminated filename argument (or a macro identifier
        // standing in for one).
        if(line.front().is_directive_hash()) {
            if(line.size() < 2 || !line[1].is_identifier()) {
                return true;
            }
            if(!takes_header_name(line[1].text(content))) {
                return true;
            }
            if(line.size() < 3) {
                return false;
            }
            // A filename token's spelling may begin with line splices; skip
            // them before inspecting the delimiter.
            auto argument = line[2].text(content).ltrim("\\\r\n \t");
            if(argument.starts_with("\"")) {
                return argument.size() >= 2 && argument.ends_with("\"");
            }
            if(argument.starts_with("<")) {
                return argument.ends_with(">");
            }
            return true;
        }

        // A module statement (import, export module, export import) is
        // complete once its last token is the semicolon. Working on tokens
        // instead of text keeps a trailing comment from hiding it.
        if(!line.front().is_identifier()) {
            return true;
        }
        auto keyword = line.front().text(content);
        if(keyword != "import" && keyword != "export") {
            return true;
        }
        return line.back().kind == clang::tok::semi;
    };

    while(true) {
        auto token = lexer.advance();
        bool at_end = token.is_eof() || token.range.begin >= bound;
        if((at_end || token.is_at_start_of_line) && !line.empty()) {
            if(!line_complete()) {
                return false;
            }
            line.clear();
        }
        if(at_end) {
            break;
        }
        if(!token.is_eod()) {
            line.push_back(token);
        }
    }

    return true;
}

}  // namespace clice
