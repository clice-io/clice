#include "CompilationUnitImpl.h"
#include "Index/USR.h"
#include "AST/Utility.h"

namespace clice {

CompilationUnit::~CompilationUnit() {
    if(impl && impl->action) {
        auto instance = impl->instance.get();
        // We already notified the pp of end-of-file earlier, so detach it first.
        // We must keep it alive until after EndSourceFile(), Sema relies on this.
        std::shared_ptr<clang::Preprocessor> pp = instance->getPreprocessorPtr();
        instance->setPreprocessor(nullptr);  // Detach so we don't send EOF again
        impl->action->EndSourceFile();
    }

    delete impl;
}

auto CompilationUnit::file_id(llvm::StringRef file) -> clang::FileID {
    auto entry = impl->src_mgr.getFileManager().getFileRef(file);
    if(entry) {
        return impl->src_mgr.translateFile(*entry);
    }

    return clang::FileID();
}

auto CompilationUnit::decompose_location(clang::SourceLocation location)
    -> std::pair<clang::FileID, std::uint32_t> {
    assert(location.isFileID() && "Decompose macro location is meaningless!");
    return impl->src_mgr.getDecomposedLoc(location);
}

auto CompilationUnit::decompose_range(clang::SourceRange range)
    -> std::pair<clang::FileID, LocalSourceRange> {
    auto [begin, end] = range;
    assert(begin.isValid() && end.isValid() && "Invalid source range");
    assert(begin.isFileID() && end.isValid() && "Input source range should be a file range");

    if(begin == end) {
        auto [fid, offset] = decompose_location(begin);
        return {
            fid,
            {offset, offset + token_length(end)}
        };
    } else {
        auto [begin_fid, begin_offset] = decompose_location(begin);
        auto [end_fid, end_offset] = decompose_location(end);

        if(begin_fid == end_fid) {
            end_offset += token_length(end);
        } else {
            auto content = file_content(begin_fid);
            end_offset = content.size();
        }

        return {
            begin_fid,
            {begin_offset, end_offset}
        };
    }
}

auto CompilationUnit::decompose_expansion_range(clang::SourceRange range)
    -> std::pair<clang::FileID, LocalSourceRange> {
    auto [begin, end] = range;
    if(begin == end) {
        return decompose_range(expansion_location(begin));
    } else {
        return decompose_range(
            clang::SourceRange(expansion_location(begin), expansion_location(end)));
    }
}

auto CompilationUnit::file_id(clang::SourceLocation location) -> clang::FileID {
    return impl->src_mgr.getFileID(location);
}

auto CompilationUnit::file_offset(clang::SourceLocation location) -> std::uint32_t {
    return impl->src_mgr.getFileOffset(location);
}

auto CompilationUnit::file_path(clang::FileID fid) -> llvm::StringRef {
    assert(fid.isValid() && "Invalid fid");
    if(auto it = impl->pathCache.find(fid); it != impl->pathCache.end()) {
        return it->second;
    }

    auto entry = impl->src_mgr.getFileEntryRefForID(fid);
    assert(entry && "Invalid file entry");

    llvm::SmallString<128> path;

    /// Try to get the real path of the file.
    auto name = entry->getName();
    if(auto error = llvm::sys::fs::real_path(name, path)) {
        /// If failed, use the virtual path.
        path = name;
    }
    assert(!path.empty() && "Invalid file path");

    /// Allocate the path in the storage.
    auto size = path.size();
    auto data = impl->pathStorage.Allocate<char>(size + 1);
    memcpy(data, path.data(), size);
    data[size] = '\0';

    auto [it, inserted] = impl->pathCache.try_emplace(fid, llvm::StringRef(data, size));
    assert(inserted && "File path already exists");
    return it->second;
}

auto CompilationUnit::file_content(clang::FileID fid) -> llvm::StringRef {
    return impl->src_mgr.getBufferData(fid);
}

auto CompilationUnit::interested_file() -> clang::FileID {
    return impl->interested;
}

auto CompilationUnit::interested_content() -> llvm::StringRef {
    return file_content(impl->interested);
}

auto CompilationUnit::start_location(clang::FileID fid) -> clang::SourceLocation {
    return impl->src_mgr.getLocForStartOfFile(fid);
}

auto CompilationUnit::end_location(clang::FileID fid) -> clang::SourceLocation {
    return impl->src_mgr.getLocForEndOfFile(fid);
}

auto CompilationUnit::spelling_location(clang::SourceLocation loc) -> clang::SourceLocation {
    return impl->src_mgr.getSpellingLoc(loc);
}

auto CompilationUnit::expansion_location(clang::SourceLocation location) -> clang::SourceLocation {
    return impl->src_mgr.getExpansionLoc(location);
}

auto CompilationUnit::file_location(clang::SourceLocation location) -> clang::SourceLocation {
    return impl->src_mgr.getFileLoc(location);
}

auto CompilationUnit::include_location(clang::FileID fid) -> clang::SourceLocation {
    return impl->src_mgr.getIncludeLoc(fid);
}

auto CompilationUnit::presumed_location(clang::SourceLocation location) -> clang::PresumedLoc {
    return impl->src_mgr.getPresumedLoc(location, false);
}

auto CompilationUnit::create_location(clang::FileID fid, std::uint32_t offset)
    -> clang::SourceLocation {
    return impl->src_mgr.getComposedLoc(fid, offset);
}

auto CompilationUnit::spelled_tokens(clang::FileID fid) -> TokenRange {
    return impl->buffer->spelledTokens(fid);
}

auto CompilationUnit::spelled_tokens(clang::SourceRange range) -> TokenRange {
    auto tokens = impl->buffer->spelledForExpanded(impl->buffer->expandedTokens(range));
    if(!tokens) {
        return {};
    }

    return *tokens;
}

auto CompilationUnit::spelled_tokens_touch(clang::SourceLocation location) -> TokenRange {
    return clang::syntax::spelledTokensTouching(location, *impl->buffer);
}

auto CompilationUnit::expanded_tokens() -> TokenRange {
    return impl->buffer->expandedTokens();
}

auto CompilationUnit::expanded_tokens(clang::SourceRange range) -> TokenRange {
    return impl->buffer->expandedTokens(range);
}

auto CompilationUnit::expansions_overlapping(TokenRange spelled_tokens)
    -> std::vector<clang::syntax::TokenBuffer::Expansion> {
    return impl->buffer->expansionsOverlapping(spelled_tokens);
}

auto CompilationUnit::token_length(clang::SourceLocation location) -> std::uint32_t {
    return clang::Lexer::MeasureTokenLength(location, impl->src_mgr, impl->instance->getLangOpts());
}

auto CompilationUnit::token_spelling(clang::SourceLocation location) -> llvm::StringRef {
    return llvm::StringRef(impl->src_mgr.getCharacterData(location), token_length(location));
}

auto CompilationUnit::module_name() -> llvm::StringRef {
    return impl->instance->getPreprocessor().getNamedModuleName();
}

bool CompilationUnit::is_module_interface_unit() {
    return impl->instance->getPreprocessor().isInNamedInterfaceUnit();
}

clang::LangOptions& CompilationUnit::lang_options() {
    return impl->instance->getLangOpts();
}

std::vector<std::string> CompilationUnit::deps() {
    llvm::StringSet<> deps;

    /// FIXME: consider `#embed` and `__has_embed`.

    for(auto& [fid, diretive]: directives()) {
        for(auto& include: diretive.includes) {
            if(!include.skipped) {
                deps.try_emplace(file_path(include.fid));
            }
        }

        for(auto& hasInclude: diretive.has_includes) {
            if(hasInclude.fid.isValid()) {
                deps.try_emplace(file_path(hasInclude.fid));
            }
        }
    }

    std::vector<std::string> result;

    for(auto& deps: deps) {
        result.emplace_back(deps.getKey().str());
    }

    return result;
}

index::SymbolID CompilationUnit::getSymbolID(const clang::NamedDecl* decl) {
    uint64_t hash;
    auto iter = impl->symbolHashCache.find(decl);
    if(iter != impl->symbolHashCache.end()) {
        hash = iter->second;
    } else {
        llvm::SmallString<128> USR;
        index::generateUSRForDecl(decl, USR);
        hash = llvm::xxh3_64bits(USR);
        impl->symbolHashCache.try_emplace(decl, hash);
    }
    return index::SymbolID{hash, ast::name_of(decl)};
}

index::SymbolID CompilationUnit::getSymbolID(const clang::MacroInfo* macro) {
    std::uint64_t hash;
    auto name = token_spelling(macro->getDefinitionLoc());
    auto iter = impl->symbolHashCache.find(macro);
    if(iter != impl->symbolHashCache.end()) {
        hash = iter->second;
    } else {
        llvm::SmallString<128> USR;
        index::generateUSRForMacro(name, macro->getDefinitionLoc(), impl->src_mgr, USR);
        hash = llvm::xxh3_64bits(USR);
        impl->symbolHashCache.try_emplace(macro, hash);
    }
    return index::SymbolID{hash, name.str()};
}

bool CompilationUnit::is_builtin_file(clang::FileID fid) {
    auto path = file_path(fid);
    return path == "<built-in>" || path == "<command line>" || path == "<scratch space>";
}

auto CompilationUnit::diagnostics() -> llvm::ArrayRef<Diagnostic> {
    return *impl->diagnostics;
}

auto CompilationUnit::top_level_decls() -> llvm::ArrayRef<clang::Decl*> {
    return impl->top_level_decls;
}

const llvm::DenseSet<clang::FileID>& CompilationUnit::files() {
    if(impl->allFiles.empty()) {
        /// FIXME: handle preamble and embed file id.
        for(auto& [fid, diretive]: directives()) {
            for(auto& include: diretive.includes) {
                if(!include.skipped) {
                    impl->allFiles.insert(include.fid);
                }
            }
        }
        impl->allFiles.insert(impl->src_mgr.getMainFileID());
    }
    return impl->allFiles;
}

clang::TranslationUnitDecl* CompilationUnit::tu() {
    return impl->instance->getASTContext().getTranslationUnitDecl();
}

llvm::DenseMap<clang::FileID, Directive>& CompilationUnit::directives() {
    return impl->m_directives;
}

TemplateResolver& CompilationUnit::resolver() {
    assert(impl->m_resolver && "Template resolver is not available");
    return *impl->m_resolver;
}

clang::ASTContext& CompilationUnit::context() {
    return impl->instance->getASTContext();
}

clang::syntax::TokenBuffer& CompilationUnit::token_buffer() {
    return *impl->buffer;
}

}  // namespace clice
