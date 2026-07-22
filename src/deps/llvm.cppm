module;

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/StringTable.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/thread.h"
#include "llvm/Support/xxhash.h"
#include "llvm/TargetParser/Host.h"

export module llvm;

export namespace llvm {

using ::llvm::APSInt;
using ::llvm::ArrayRef;
using ::llvm::BitVector;
using ::llvm::BumpPtrAllocator;
using ::llvm::DenseMap;
using ::llvm::DenseMapInfo;
using ::llvm::DenseSet;
using ::llvm::Error;
using ::llvm::ErrorOr;
using ::llvm::FormattedNumber;
using ::llvm::IntrusiveRefCntPtr;
using ::llvm::ListSeparator;
using ::llvm::MemoryBuffer;
using ::llvm::MutableArrayRef;
using ::llvm::SHA256;
using ::llvm::SaveAndRestore;
using ::llvm::SmallDenseMap;
using ::llvm::SmallPtrSet;
using ::llvm::SmallSet;
using ::llvm::SmallString;
using ::llvm::SmallVector;
using ::llvm::SmallVectorImpl;
using ::llvm::SplitString;
using ::llvm::StringLiteral;
using ::llvm::StringMap;
using ::llvm::StringRef;
using ::llvm::StringSaver;
using ::llvm::StringSet;
using ::llvm::StringSwitch;
using ::llvm::StringTable;
using ::llvm::Twine;
using ::llvm::all_of;
using ::llvm::any_of;
using ::llvm::arrayRefFromStringRef;
using ::llvm::cast;
using ::llvm::consumeError;
using ::llvm::dyn_cast;
using ::llvm::dyn_cast_or_null;
using ::llvm::erase_if;
using ::llvm::errorToErrorCode;
using ::llvm::find;
using ::llvm::find_if;
using ::llvm::format;
using ::llvm::format_hex;
using ::llvm::formatv;
using ::llvm::function_ref;
using ::llvm::hash_combine;
using ::llvm::hash_combine_range;
using ::llvm::isAlnum;
using ::llvm::isAlpha;
using ::llvm::isDigit;
using ::llvm::isHexDigit;
using ::llvm::isPrint;
using ::llvm::isSpace;
using ::llvm::is_contained;
using ::llvm::isa;
using ::llvm::isa_and_nonnull;
using ::llvm::join;
using ::llvm::join_items;
using ::llvm::makeIntrusiveRefCnt;
using ::llvm::make_filter_range;
using ::llvm::make_range;
using ::llvm::make_scope_exit;
using ::llvm::operator!=;
using ::llvm::operator&;
using ::llvm::operator+;
using ::llvm::operator-;
using ::llvm::operator<;
using ::llvm::operator<<;
using ::llvm::operator<=;
using ::llvm::operator==;
using ::llvm::operator>;
using ::llvm::operator>=;
using ::llvm::operator^;
using ::llvm::operator|;
using ::llvm::outs;
using ::llvm::partition_point;
using ::llvm::printEscapedString;
using ::llvm::raw_fd_ostream;
using ::llvm::raw_ostream;
using ::llvm::raw_string_ostream;
using ::llvm::raw_svector_ostream;
using ::llvm::reverse;
using ::llvm::size;
using ::llvm::sort;
using ::llvm::thread;
using ::llvm::toString;
using ::llvm::to_string;
using ::llvm::unique;
using ::llvm::xxh3_128bits;
using ::llvm::xxh3_64bits;

}  // namespace llvm

export namespace llvm::cl {

using ::llvm::cl::TokenizeGNUCommandLine;
using ::llvm::cl::TokenizeWindowsCommandLineFull;

}  // namespace llvm::cl

export namespace llvm::json {

using ::llvm::json::fixUTF8;
using ::llvm::json::isUTF8;

}  // namespace llvm::json

export namespace llvm::sys {

using ::llvm::sys::AddSignalHandler;
using ::llvm::sys::PrintStackTrace;
using ::llvm::sys::PrintStackTraceOnErrorSignal;
using ::llvm::sys::Process;
using ::llvm::sys::TimePoint;
using ::llvm::sys::findProgramByName;
using ::llvm::sys::getDefaultTargetTriple;

}  // namespace llvm::sys

export namespace llvm::sys::fs {

using ::llvm::sys::fs::CD_OpenExisting;
using ::llvm::sys::fs::FA_Write;
using ::llvm::sys::fs::OF_Append;
using ::llvm::sys::fs::OF_None;
using ::llvm::sys::fs::closeFile;
using ::llvm::sys::fs::createTemporaryFile;
using ::llvm::sys::fs::createUniqueDirectory;
using ::llvm::sys::fs::create_directories;
using ::llvm::sys::fs::directory_iterator;
using ::llvm::sys::fs::equivalent;
using ::llvm::sys::fs::exists;
using ::llvm::sys::fs::file_status;
using ::llvm::sys::fs::file_type;
using ::llvm::sys::fs::getMainExecutable;
using ::llvm::sys::fs::is_directory;
using ::llvm::sys::fs::make_absolute;
using ::llvm::sys::fs::openFileForWrite;
using ::llvm::sys::fs::openNativeFile;
using ::llvm::sys::fs::real_path;
using ::llvm::sys::fs::remove;
using ::llvm::sys::fs::rename;
using ::llvm::sys::fs::setLastAccessAndModificationTime;
using ::llvm::sys::fs::status;

}  // namespace llvm::sys::fs

export namespace llvm::sys::path {

using ::llvm::sys::path::append;
using ::llvm::sys::path::extension;
using ::llvm::sys::path::filename;
using ::llvm::sys::path::is_absolute;
using ::llvm::sys::path::native;
using ::llvm::sys::path::parent_path;
using ::llvm::sys::path::remove_dots;
using ::llvm::sys::path::stem;

}  // namespace llvm::sys::path

export namespace llvm::vfs {

using ::llvm::vfs::FileSystem;
using ::llvm::vfs::InMemoryFileSystem;
using ::llvm::vfs::OverlayFileSystem;
using ::llvm::vfs::createPhysicalFileSystem;
using ::llvm::vfs::getRealFileSystem;

}  // namespace llvm::vfs
