// Lexes a line of C++ with libclang's own classes, and checks the version and
// the compression the LLVM libraries were built with.
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

int main() {
  clang::FileManager files{clang::FileSystemOptions{}};
  clang::DiagnosticOptions diagOptions;
  clang::DiagnosticsEngine diags(new clang::DiagnosticIDs, diagOptions);
  clang::SourceManager sources(diags, files);
  auto buffer = llvm::MemoryBuffer::getMemBuffer("int main() { return 42; }", "input.cpp");
  clang::FileID file = sources.createFileID(std::move(buffer));
  clang::LangOptions lang;
  lang.CPlusPlus = true;
  clang::Lexer lexer(file, sources.getBufferOrFake(file), sources, lang);
  unsigned tokens = 0;
  clang::Token token;
  do {
    lexer.LexFromRawLexer(token);
    if (token.isNot(clang::tok::eof)) ++tokens;
  } while (token.isNot(clang::tok::eof));
  std::string version = clang::getClangFullVersion();
  bool zlib = llvm::compression::zlib::isAvailable();
  bool zstd = llvm::compression::zstd::isAvailable();
  llvm::outs() << version << "\n"
               << "tokens " << tokens << " zlib " << zlib << " zstd " << zstd << "\n";
  return version.find("clang version 23") != std::string::npos && tokens == 9 && zlib && zstd ? 0 : 1;
}
