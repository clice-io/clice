module clice;

import :command.file_kind;
import :vfs.path;

namespace clice {

clang::driver::types::ID suffix_type(llvm::StringRef path) {
    namespace types = clang::driver::types;
    llvm::StringRef ext = path::extension(path);
    ext.consume_front(".");
    return ext.empty() ? types::TY_INVALID : types::lookupTypeForExtension(ext);
}

llvm::StringRef extension_language(llvm::StringRef path, bool cxx) {
    namespace types = clang::driver::types;
    auto type = suffix_type(path);
    if(type == types::TY_INVALID) {
        llvm::StringRef ext = path::extension(path);
        if(ext.empty()) {
            return cxx ? "c++-header" : "c-header";
        }
        ext.consume_front(".");
        return ext == "cuh" ? "cuda" : ext;
    }
    return types::getTypeName(cxx ? types::lookupCXXTypeForCType(type) : type);
}

bool is_header_path(llvm::StringRef path) {
    namespace types = clang::driver::types;
    auto type = suffix_type(path);
    return type == types::TY_INVALID || types::onlyPrecompileType(type);
}

bool is_source_path(llvm::StringRef path) {
    return !is_header_path(path) && clang::driver::types::isDerivedFromC(suffix_type(path));
}

bool is_other_language_path(llvm::StringRef path) {
    auto type = suffix_type(path);
    return type != clang::driver::types::TY_INVALID && !is_c_family_type(type);
}

bool is_context_header_path(llvm::StringRef path) {
    return path.ends_with(".def") || path.ends_with(".inc") || path.ends_with(".inl") ||
           path.ends_with(".tpp") || path.ends_with(".ipp") || path.ends_with(".tcc") ||
           path.ends_with(".txx");
}

bool is_c_family_type(clang::driver::types::ID type) {
    namespace types = clang::driver::types;
    return types::isAcceptedByClang(type) && type != types::TY_Asm && !types::isLLVMIR(type) &&
           !ranges::contains(
               std::array{types::TY_AST, types::TY_ModuleFile, types::TY_PCH, types::TY_API_INFO},
               type);
}

bool is_c_family_file(llvm::StringRef filename) {
    namespace types = clang::driver::types;
    llvm::StringRef ext = path::extension(filename);
    if(ext.empty()) {
        return false;
    }
    /// CUDA's header convention is missing from clang's extension table
    /// (classifying one needs -x cuda), but it is C-family.
    if(ext == ".cuh") {
        return true;
    }
    /// Drop the leading dot: ".cpp" -> "cpp".
    return is_c_family_type(types::lookupTypeForExtension(ext.drop_front()));
}

/// Whether `type` is C++ or a C++ module unit: the two share flags, so
/// each lends to the other.
static bool cxx_or_module(clang::driver::types::ID type) {
    namespace types = clang::driver::types;
    return type == types::TY_CXX || type == types::TY_CXXModule;
}

bool compatible(llvm::StringRef path, llvm::StringRef unit, clang::driver::types::ID language) {
    namespace types = clang::driver::types;
    auto file = suffix_type(path);
    if(file == types::TY_INVALID) {
        return path::extension(path) != ".cuh" || types::isCuda(language) || types::isHIP(language);
    }
    if(file == types::TY_CHeader) {
        return true;
    }
    if(types::onlyPrecompileType(file) && types::isCXX(file)) {
        return types::isCXX(language);
    }
    return file == language || (cxx_or_module(file) && cxx_or_module(language)) ||
           (file == suffix_type(unit) && language == types::lookupCXXTypeForCType(file));
}

}  // namespace clice
