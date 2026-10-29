// What clice takes from the libraries modules/ wraps, as a program
// scripts/modules.py indexes to write the rest of modules/: the switches
// clice defines ahead of them, the standard headers it reaches C library
// names through, the libraries' headers it uses and the ones it still
// includes itself, the C library names only those headers bring, and the
// macros it uses: expanded when the expansion names the C library, an
// #ifdef each otherwise. What clice starts to use that none of these brings
// goes here first, then modules/ is regenerated.

#define ROARING_EXCEPTIONS 0
#define ROARING_TERMINATE(message) std::abort()

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <format>
#include <algorithm>
#include <utility>
#include <memory>
#include <chrono>
#include <cassert>
#include <expected>
#include <ranges>
#include <cstddef>
#include <tuple>
#include <functional>
#include <string_view>
#include <map>
#include <cstdlib>
#include <array>
#include <set>
#include <type_traits>
#include <thread>
#include <span>
#include <print>
#include <unordered_map>
#include <system_error>
#include <cstring>
#include <atomic>
#include <variant>
#include <limits>
#include <deque>
#include <csignal>
#include <mutex>
#include <concepts>
#include <cerrno>
#include <sstream>
#include <bit>
#include <source_location>
#include <numeric>
#include <list>
#include <iterator>
#include <initializer_list>
#include <compare>
#include <random>
#include <ctime>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <coroutine>
#include <cmath>
#include <cctype>
#include <bitset>

#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyDiagnosticConsumer.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang-tidy/ClangTidyOptions.h"
#include "clang-tidy/utils/DesignatedInitializers.h"
#include "clang/AST/APValue.h"
#include "clang/AST/ASTConcept.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RawCommentList.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/TemplateName.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/AllDiagnostics.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticLex.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/LangStandard.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/OperatorKinds.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/Stack.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Basic/Version.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Driver/Types.h"
#include "clang/Format/Format.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/DependencyDirectivesScanner.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/Token.h"
#include "clang/Options/OptionUtils.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/DeclSpec.h"
#include "clang/Sema/Designator.h"
#include "clang/Sema/HeuristicResolver.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaCodeCompletion.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Inclusions/StandardLibrary.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringTable.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/thread.h"
#include "llvm/Support/xxhash.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/RISCVISAInfo.h"
#include "llvm/TargetParser/Triple.h"
#include "roaring/roaring.hh"
#include "lmdb.h"
#include "spdlog/details/log_msg.h"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/spdlog.h"
#include "simdjson.h"
#include "kota/async/async.h"
#include "kota/fs/cygwin.h"
#include "kota/fs/directory.h"
#include "kota/fs/file.h"
#include "kota/fs/mapping.h"
#include "kota/fs/path.h"
#include "kota/fs/resolve.h"
#include "kota/fs/status.h"
#include "kota/fs/temp.h"
#include "kota/fs/volume.h"
#include "kota/fs/windows.h"
#include "kota/support/buffer_ref.h"
#include "kota/support/small_string.h"
#include "kota/support/string_list_ref.h"
#include "kota/support/utf16.h"
#include "kota/sys/console.h"
#include "kota/sys/dirs.h"
#include "kota/sys/env.h"
#include "kota/sys/info.h"
#include "kota/sys/process.h"
#include "kota/sys/spawn.h"
#include "kota/sys/windows.h"
#include "kota/codec/bincode/bincode.h"
#include "kota/codec/blob/blob.h"
#include "kota/codec/dyn/decode.h"
#include "kota/codec/dyn/dyn.h"
#include "kota/codec/json/json.h"
#include "kota/codec/json/schema.h"
#include "kota/codec/macro.h"
#include "kota/codec/toml/toml.h"
#include "kota/codec/visit/config.h"
#include "kota/deco/deco.h"
#include <kota/deco/option.h>
#include "kota/ipc/codec/bincode.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/progress.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/text.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/ipc/peer.h"
#include "kota/ipc/peer/bincode.h"
#include "kota/ipc/peer/json.h"
#include "kota/ipc/protocol.h"
#include "kota/ipc/recording_transport.h"
#include "kota/ipc/transport.h"
#include "kota/meta/annotation.h"
#include "kota/meta/enum.h"
#include "kota/meta/repr.h"
#include "kota/meta/struct.h"
#include "kota/support/glob_pattern.h"
#include "kota/zest/async.h"
#include "kota/zest/zest.h"
#ifdef _WIN32
#include "llvm/Support/WindowsError.h"
#endif

#include "clang-tidy/ClangTidyForceLinker.h"
#include "kota/ipc/framing.h"

using kota::ipc::default_max_payload;

using ::fileno;
#ifdef _WIN32
using ::_putenv_s;
using ::localtime_s;
#else
using ::kill;
using ::localtime_r;
using ::off_t;
using ::pid_t;
using ::putenv;
using ::setenv;
using ::ssize_t;
#endif

[[maybe_unused]] static void expand() {
    std::signal(SIGINT, SIG_IGN);
    std::signal(SIGINT, SIG_DFL);
    errno = 0;
    std::fputs("", stdout);
    std::fputs("", stderr);
    assert(true);
}

#ifdef DecoFlag
#endif
#ifdef DecoInput
#endif
#ifdef DecoKV
#endif
#ifdef EINTR
#endif
#ifdef EINVAL
#endif
#ifdef EPERM
#endif
#ifdef EPIPE
#endif
#ifdef ESRCH
#endif
#ifdef INT_MAX
#endif
#ifdef KOTATSU_ANNOTATE
#endif
#ifdef LLVM_BUILTIN_TRAP
#endif
#ifdef LLVM_DEFAULT_TARGET_TRIPLE
#endif
#ifdef MDB_CORRUPTED
#endif
#ifdef MDB_CP_COMPACT
#endif
#ifdef MDB_INVALID
#endif
#ifdef MDB_MAP_FULL
#endif
#ifdef MDB_MAP_RESIZED
#endif
#ifdef MDB_NOSUBDIR
#endif
#ifdef MDB_NOTFOUND
#endif
#ifdef MDB_NOTLS
#endif
#ifdef MDB_PAGE_NOTFOUND
#endif
#ifdef MDB_RDONLY
#endif
#ifdef MDB_VERSION_MISMATCH
#endif
#ifdef SIGCONT
#endif
#ifdef SIGHUP
#endif
#ifdef SIGINT
#endif
#ifdef SIGPIPE
#endif
#ifdef SIGSTOP
#endif
#ifdef SIGTERM
#endif
#ifdef SIGXFSZ
#endif
#ifdef SIZE_MAX
#endif
#ifdef UINT32_MAX
#endif
#ifdef ZASSERT
#endif
#ifdef ZEST_CASE
#endif
#ifdef ZEST_SUITE
#endif
#ifdef ZEXPECT
#endif
#ifdef __GLIBC__
#endif
#ifdef va_start
#endif
