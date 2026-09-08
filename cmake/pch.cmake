include_guard()

# The third-party headers most translation units pull in: the standard
# library headers included by at least half of all units, and the LLVM,
# clang and kotatsu headers included by at least half of the units that use
# clang at all (they arrive through a handful of project headers anyway).
# Measured per unit: 30-60% less compile time for ordinary units, 7-20% for
# the codec-heavy ones. -fpch-instantiate-templates moves the template
# instantiations the headers trigger into the PCH as well; without it the
# gain roughly halves. -fno-pch-timestamp keeps header mtimes out of the
# PCH, so ccache can hit across fresh checkouts (CMake does not add it).
# CMAKE_DISABLE_PRECOMPILE_HEADERS=ON turns this off.
set(CLICE_PCH_HEADERS
    <algorithm>
    <array>
    <bit>
    <cassert>
    <charconv>
    <compare>
    <concepts>
    <cstddef>
    <cstdint>
    <cstdio>
    <cstdlib>
    <cstring>
    <exception>
    <format>
    <functional>
    <initializer_list>
    <iterator>
    <limits>
    <memory>
    <numbers>
    <optional>
    <ostream>
    <span>
    <string>
    <string_view>
    <system_error>
    <tuple>
    <type_traits>
    <unordered_map>
    <utility>
    <vector>

    <llvm/ADT/APFloat.h>
    <llvm/ADT/APSInt.h>
    <llvm/ADT/ArrayRef.h>
    <llvm/ADT/BitVector.h>
    <llvm/ADT/DenseMap.h>
    <llvm/ADT/DenseSet.h>
    <llvm/ADT/FoldingSet.h>
    <llvm/ADT/FunctionExtras.h>
    <llvm/ADT/MapVector.h>
    <llvm/ADT/PointerUnion.h>
    <llvm/ADT/SetVector.h>
    <llvm/ADT/SmallString.h>
    <llvm/ADT/SmallVector.h>
    <llvm/ADT/STLExtras.h>
    <llvm/ADT/StringExtras.h>
    <llvm/ADT/StringMap.h>
    <llvm/ADT/StringRef.h>
    <llvm/ADT/StringSet.h>
    <llvm/ADT/TinyPtrVector.h>
    <llvm/ADT/Twine.h>
    <llvm/Support/Allocator.h>
    <llvm/Support/Casting.h>
    <llvm/Support/Chrono.h>
    <llvm/Support/Compression.h>
    <llvm/Support/Debug.h>
    <llvm/Support/Error.h>
    <llvm/Support/ErrorOr.h>
    <llvm/Support/FileSystem.h>
    <llvm/Support/Format.h>
    <llvm/Support/MD5.h>
    <llvm/Support/MemoryBuffer.h>
    <llvm/Support/Path.h>
    <llvm/Support/PrettyStackTrace.h>
    <llvm/Support/SourceMgr.h>
    <llvm/Support/VirtualFileSystem.h>
    <llvm/Support/raw_ostream.h>
    <llvm/Support/xxhash.h>
    <llvm/TargetParser/Triple.h>

    <clang/Basic/Builtins.h>
    <clang/Basic/CharInfo.h>
    <clang/Basic/Diagnostic.h>
    <clang/Basic/DiagnosticIDs.h>
    <clang/Basic/DiagnosticOptions.h>
    <clang/Basic/FileManager.h>
    <clang/Basic/IdentifierTable.h>
    <clang/Basic/LangOptions.h>
    <clang/Basic/SourceManager.h>
    <clang/Basic/TokenKinds.h>
    <clang/AST/APValue.h>
    <clang/AST/ASTConcept.h>
    <clang/AST/ASTContext.h>
    <clang/AST/Decl.h>
    <clang/AST/DeclBase.h>
    <clang/AST/DeclCXX.h>
    <clang/AST/DeclTemplate.h>
    <clang/AST/Expr.h>
    <clang/AST/ExprCXX.h>
    <clang/AST/ExternalASTSource.h>
    <clang/AST/NestedNameSpecifier.h>
    <clang/AST/PrettyPrinter.h>
    <clang/AST/RawCommentList.h>
    <clang/AST/Stmt.h>
    <clang/AST/StmtCXX.h>
    <clang/AST/TemplateBase.h>
    <clang/AST/TemplateName.h>
    <clang/AST/Type.h>
    <clang/AST/TypeLoc.h>
    <clang/Lex/DependencyDirectivesScanner.h>
    <clang/Lex/MacroInfo.h>
    <clang/Lex/Token.h>
    <clang/Tooling/Syntax/Tokens.h>

    <kota/codec/macro.h>
    <kota/codec/visit/decode.h>
    <kota/codec/visit/encode.h>
    <kota/ipc/lsp/protocol.h>
    <kota/ipc/protocol.h>
    <kota/meta/repr.h>
    <kota/meta/schema.h>
    <kota/meta/type_info.h>
    <kota/support/naming.h>
    <kota/support/ranges.h>
    <kota/support/string_ref.h>

    <spdlog/fmt/fmt.h>
    <spdlog/spdlog.h>
)

function(clice_precompile_headers target)
    target_precompile_headers(${target} PRIVATE ${CLICE_PCH_HEADERS})
    target_compile_options(${target} PRIVATE
        "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-fpch-instantiate-templates;-Xclang;-fno-pch-timestamp>")
endfunction()
