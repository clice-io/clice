module;

#if defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)
#include "linux-x64/llvm.fragment.h"
#elif defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)
#include "linux-x64-debug/llvm.fragment.h"
#elif defined(__linux__) && defined(__aarch64__)
#include "linux-arm64/llvm.fragment.h"
#elif defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)
#include "macos-arm64/llvm.fragment.h"
#elif defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)
#include "macos-arm64-debug/llvm.fragment.h"
#elif defined(__APPLE__) && defined(__x86_64__)
#include "macos-x64/llvm.fragment.h"
#else
#error "no configuration merged matches this compilation"
#endif

export module llvm;

export using ::LLVMAttributeRef;
export using ::LLVMBasicBlockRef;
export using ::LLVMBinaryRef;
export using ::LLVMBool;
export using ::LLVMBuilderRef;
export using ::LLVMCantFail;
export using ::LLVMComdat;
export using ::LLVMComdatRef;
export using ::LLVMConsumeError;
export using ::LLVMContextRef;
export using ::LLVMCreateStringError;
export using ::LLVMDIBuilderRef;
export using ::LLVMDbgRecordRef;
export using ::LLVMDiagnosticInfoRef;
export using ::LLVMDisposeErrorMessage;
export using ::LLVMErrorRef;
export using ::LLVMErrorTypeId;
export using ::LLVMGetErrorMessage;
export using ::LLVMGetErrorTypeId;
export using ::LLVMGetStringErrorTypeId;
export using ::LLVMJITEventListenerRef;
export using ::LLVMMemoryBufferRef;
export using ::LLVMMetadataRef;
export using ::LLVMModuleFlagEntry;
export using ::LLVMModuleProviderRef;
export using ::LLVMModuleRef;
export using ::LLVMNamedMDNodeRef;
export using ::LLVMOpaqueAttributeRef;
export using ::LLVMOpaqueBasicBlock;
export using ::LLVMOpaqueBinary;
export using ::LLVMOpaqueBuilder;
export using ::LLVMOpaqueContext;
export using ::LLVMOpaqueDIBuilder;
export using ::LLVMOpaqueDbgRecord;
export using ::LLVMOpaqueDiagnosticInfo;
export using ::LLVMOpaqueError;
export using ::LLVMOpaqueJITEventListener;
export using ::LLVMOpaqueMemoryBuffer;
export using ::LLVMOpaqueMetadata;
export using ::LLVMOpaqueModule;
export using ::LLVMOpaqueModuleFlagEntry;
export using ::LLVMOpaqueModuleProvider;
export using ::LLVMOpaqueNamedMDNode;
export using ::LLVMOpaqueOperandBundle;
export using ::LLVMOpaquePassManager;
export using ::LLVMOpaqueType;
export using ::LLVMOpaqueUse;
export using ::LLVMOpaqueValue;
export using ::LLVMOpaqueValueMetadataEntry;
export using ::LLVMOperandBundleRef;
export using ::LLVMPassManagerRef;
export using ::LLVMTypeRef;
export using ::LLVMUseRef;
export using ::LLVMValueMetadataEntry;
export using ::LLVMValueRef;
export using ::llvm_regex;
export using ::operator delete;
export using ::operator new;

export namespace llvm {
using ::llvm::APFixedPoint;
using ::llvm::APFloat;
using ::llvm::APFloatBase;
using ::llvm::APInt;
using ::llvm::APSInt;
using ::llvm::AbsoluteDifference;
using ::llvm::AbsoluteValue;
using ::llvm::AddOverflow;
using ::llvm::Align;
using ::llvm::AlignStyle;
using ::llvm::AlignedCharArrayUnion;
using ::llvm::AllocTokenMetadata;
using ::llvm::AllocTokenMode;
using ::llvm::AllocatorBase;
using ::llvm::ArrayRef;
using ::llvm::ArrayType;
using ::llvm::AsanCtorKind;
using ::llvm::AsanDetectStackUseAfterReturnMode;
using ::llvm::AsanDtorKind;
using ::llvm::AtomicOrdering;
using ::llvm::AtomicOrderingCABI;
using ::llvm::BWH_CPUTypeField;
using ::llvm::BWH_HeaderSize;
using ::llvm::BWH_MagicField;
using ::llvm::BWH_OffsetField;
using ::llvm::BWH_SizeField;
using ::llvm::BWH_VersionField;
using ::llvm::BasicBlockSection;
using ::llvm::BitCodeAbbrev;
using ::llvm::BitCodeAbbrevOp;
using ::llvm::BitVector;
using ::llvm::BitWidth;
using ::llvm::BitstreamWrapperHeader;
using ::llvm::BitstreamWriter;
using ::llvm::Boolean;
using ::llvm::BumpPtrAllocator;
using ::llvm::BumpPtrAllocatorImpl;
using ::llvm::BuryPointer;
using ::llvm::ByteType;
using ::llvm::COV_2;
using ::llvm::COV_3;
using ::llvm::COV_4;
using ::llvm::COV_5;
using ::llvm::COV_6;
using ::llvm::COV_None;
using ::llvm::CTLog2;
using ::llvm::CachedHashString;
using ::llvm::CachedHashStringRef;
using ::llvm::CalculateSmallVectorDefaultInlinedElements;
using ::llvm::CastIfPresentTo;
using ::llvm::CastInfo;
using ::llvm::CastIsPossible;
using ::llvm::CastTo;
using ::llvm::CodeGenFileType;
using ::llvm::CodeGenOptLevel;
using ::llvm::CodeObjectVersionKind;
using ::llvm::ConstStrippingForwardingCast;
using ::llvm::ConstantLog2;
using ::llvm::ContextualFoldingSet;
using ::llvm::ContextualFoldingSetTrait;
using ::llvm::ControlFlowGuardMechanism;
using ::llvm::ControlFlowGuardMode;
using ::llvm::ConversionFlags;
using ::llvm::ConversionResult;
using ::llvm::ConvertCodePointToUTF8;
using ::llvm::ConvertUTF16toUTF32;
using ::llvm::ConvertUTF16toUTF8;
using ::llvm::ConvertUTF32toUTF16;
using ::llvm::ConvertUTF32toUTF8;
using ::llvm::ConvertUTF8toUTF16;
using ::llvm::ConvertUTF8toUTF32;
using ::llvm::ConvertUTF8toUTF32Partial;
using ::llvm::ConvertUTF8toWide;
using ::llvm::DebugCompressionType;
using ::llvm::DebugEpochBase;
using ::llvm::DebugFlag;
using ::llvm::DebuggerKind;
using ::llvm::DefaultAllocTokenMode;
using ::llvm::DefaultContextualFoldingSetTrait;
using ::llvm::DefaultDoCastIfPossible;
using ::llvm::DefaultFoldingSetTrait;
using ::llvm::DelimitedScope;
using ::llvm::DenormalFPEnv;
using ::llvm::DenormalMode;
using ::llvm::DenseMap;
using ::llvm::DenseMapBase;
using ::llvm::DenseMapInfo;
using ::llvm::DenseMapIterator;
using ::llvm::DenseSet;
using ::llvm::DictScope;
using ::llvm::Duration;
using ::llvm::DynCastIfPresentTo;
using ::llvm::DynCastTo;
using ::llvm::DynamicAPInt;
using ::llvm::EABI;
using ::llvm::ECError;
using ::llvm::ElementCount;
using ::llvm::EmitDwarfUnwindType;
using ::llvm::EmptyStringSetTag;
using ::llvm::EnableDebugBuffering;
using ::llvm::EnableIfConvertibleToInputIterator;
using ::llvm::EnablePrettyStackTrace;
using ::llvm::EnablePrettyStackTraceOnSigInfoForThisThread;
using ::llvm::EnumString;
using ::llvm::EnumStringDef;
using ::llvm::EnumStrings;
using ::llvm::EnumStringsStorage;
using ::llvm::EquivalenceClasses;
using ::llvm::Error;
using ::llvm::ErrorAsOutParameter;
using ::llvm::ErrorHandlerTraits;
using ::llvm::ErrorInfo;
using ::llvm::ErrorInfoBase;
using ::llvm::ErrorList;
using ::llvm::ErrorOr;
using ::llvm::ErrorSuccess;
using ::llvm::ExceptionHandling;
using ::llvm::ExitOnError;
using ::llvm::Expected;
using ::llvm::ExpectedAsOutParameter;
using ::llvm::FPClassTest;
using ::llvm::FailureOr;
using ::llvm::FastFoldingSetNode;
using ::llvm::FileCollector;
using ::llvm::FileCollectorBase;
using ::llvm::FileCollectorFileSystem;
using ::llvm::FileError;
using ::llvm::FirstIndexOfType;
using ::llvm::FixedPointSemantics;
using ::llvm::FixedVectorType;
using ::llvm::FlagEntry;
using ::llvm::FloatStyle;
using ::llvm::FmtAlign;
using ::llvm::FoldingSet;
using ::llvm::FoldingSetBase;
using ::llvm::FoldingSetBucketIterator;
using ::llvm::FoldingSetBucketIteratorImpl;
using ::llvm::FoldingSetImpl;
using ::llvm::FoldingSetIterator;
using ::llvm::FoldingSetIteratorImpl;
using ::llvm::FoldingSetNode;
using ::llvm::FoldingSetNodeID;
using ::llvm::FoldingSetNodeIDRef;
using ::llvm::FoldingSetNodeWrapper;
using ::llvm::FoldingSetTrait;
using ::llvm::FoldingSetVector;
using ::llvm::FormattedBytes;
using ::llvm::FormattedNumber;
using ::llvm::FormattedString;
using ::llvm::ForwardToPointerCast;
using ::llvm::FramePointerKind;
using ::llvm::FreeDeleter;
using ::llvm::FunctionCallee;
using ::llvm::FunctionPointerLikeTypeTraits;
using ::llvm::FunctionReturnThunksKind;
using ::llvm::FunctionType;
using ::llvm::GlobalISelAbortMode;
using ::llvm::GraphHasNodeNumbers;
using ::llvm::GraphTraits;
using ::llvm::HasIteratorTag;
using ::llvm::HashBuilder;
using ::llvm::HashBuilderBase;
using ::llvm::HexNumber;
using ::llvm::HexPrintStyle;
using ::llvm::Hi_32;
using ::llvm::IntegerStyle;
using ::llvm::IntegerType;
using ::llvm::IntrusiveRefCntPtr;
using ::llvm::IntrusiveRefCntPtrInfo;
using ::llvm::Inverse;
using ::llvm::IsNullable;
using ::llvm::IsSingleCodeUnitUTF16Codepoint;
using ::llvm::IsSingleCodeUnitUTF32Codepoint;
using ::llvm::IsSingleCodeUnitUTF8Codepoint;
using ::llvm::IsaAndPresentPred;
using ::llvm::IsaPred;
using ::llvm::JSONScopedPrinter;
using ::llvm::KCFIHashAlgorithm;
using ::llvm::LLVMContext;
using ::llvm::ListScope;
using ::llvm::ListSeparator;
using ::llvm::Lo_32;
using ::llvm::LoadIntFromMemory;
using ::llvm::Log2;
using ::llvm::Log2_32;
using ::llvm::Log2_32_Ceil;
using ::llvm::Log2_64;
using ::llvm::Log2_64_Ceil;
using ::llvm::LogicalResult;
using ::llvm::MCTargetOptions;
using ::llvm::MD5;
using ::llvm::MD5Hash;
using ::llvm::MachineFunction;
using ::llvm::Make_64;
using ::llvm::MallocAllocator;
using ::llvm::ManagedStatic;
using ::llvm::ManagedStaticBase;
using ::llvm::MapVector;
using ::llvm::MaybeAlign;
using ::llvm::MemoryBuffer;
using ::llvm::MemoryBufferRef;
using ::llvm::MinAlign;
using ::llvm::MulOverflow;
using ::llvm::MutableArrayRef;
using ::llvm::NamedRegionTimer;
using ::llvm::NextPowerOf2;
using ::llvm::NullableValueCastFailed;
using ::llvm::NumDigitsBase10;
using ::llvm::OptionalValueCast;
using ::llvm::PagedVector;
using ::llvm::ParseResult;
using ::llvm::PointerIntPair;
using ::llvm::PointerIntPairInfo;
using ::llvm::PointerLikeTypeTraits;
using ::llvm::PointerType;
using ::llvm::PointerUnion;
using ::llvm::PowerOf2Ceil;
using ::llvm::PrettyStackTraceEntry;
using ::llvm::PrettyStackTraceFormat;
using ::llvm::PrettyStackTraceProgram;
using ::llvm::PrettyStackTraceString;
using ::llvm::RTTIExtends;
using ::llvm::RTTIRoot;
using ::llvm::RefCountedBase;
using ::llvm::Regex;
using ::llvm::Registry;
using ::llvm::RelocSectionSymType;
using ::llvm::ReplacementItem;
using ::llvm::ReplacementType;
using ::llvm::RestorePrettyStackState;
using ::llvm::RoundingMode;
using ::llvm::SHA256;
using ::llvm::SMDiagnostic;
using ::llvm::SMFixIt;
using ::llvm::SMLoc;
using ::llvm::SMRange;
using ::llvm::SameType;
using ::llvm::SaturatingAdd;
using ::llvm::SaturatingMultiply;
using ::llvm::SaturatingMultiplyAdd;
using ::llvm::SaveAndRestore;
using ::llvm::SavePrettyStackState;
using ::llvm::ScalableVectorType;
using ::llvm::ScopedFatalErrorHandler;
using ::llvm::ScopedPrinter;
using ::llvm::SetThreadPriorityResult;
using ::llvm::SetVector;
using ::llvm::SignExtend32;
using ::llvm::SignExtend64;
using ::llvm::SimpleRegistryEntry;
using ::llvm::SmallBitVector;
using ::llvm::SmallDenseMap;
using ::llvm::SmallDenseSet;
using ::llvm::SmallMapVector;
using ::llvm::SmallPtrSet;
using ::llvm::SmallPtrSetImpl;
using ::llvm::SmallPtrSetImplBase;
using ::llvm::SmallPtrSetIterator;
using ::llvm::SmallPtrSetIteratorImpl;
using ::llvm::SmallSet;
using ::llvm::SmallSetIterator;
using ::llvm::SmallSetVector;
using ::llvm::SmallString;
using ::llvm::SmallVector;
using ::llvm::SmallVectorAlignmentAndSize;
using ::llvm::SmallVectorBase;
using ::llvm::SmallVectorImpl;
using ::llvm::SmallVectorSizeType;
using ::llvm::SmallVectorStorage;
using ::llvm::SmallVectorTemplateCommon;
using ::llvm::SourceMgr;
using ::llvm::SpecificBumpPtrAllocator;
using ::llvm::SplitString;
using ::llvm::SplittingIterator;
using ::llvm::StackOffset;
using ::llvm::StaticCastTo;
using ::llvm::StoreIntToMemory;
using ::llvm::StringError;
using ::llvm::StringLiteral;
using ::llvm::StringMap;
using ::llvm::StringMapEntry;
using ::llvm::StringMapEntryBase;
using ::llvm::StringMapEntryStorage;
using ::llvm::StringMapImpl;
using ::llvm::StringMapIterBase;
using ::llvm::StringMapKeyIterator;
using ::llvm::StringRef;
using ::llvm::StringSaver;
using ::llvm::StringSet;
using ::llvm::StringSwitch;
using ::llvm::StringTable;
using ::llvm::StructType;
using ::llvm::SubOverflow;
using ::llvm::SwiftAsyncFramePointerMode;
using ::llvm::TargetExtType;
using ::llvm::TargetLibraryInfoImpl;
using ::llvm::TargetOptions;
using ::llvm::ThreadPoolStrategy;
using ::llvm::ThreadPriority;
using ::llvm::ThreadSafeRefCountedBase;
using ::llvm::TimeRecord;
using ::llvm::TimeRegion;
using ::llvm::Timer;
using ::llvm::TimerGlobals;
using ::llvm::TimerGroup;
using ::llvm::TinyPtrVector;
using ::llvm::TrailingObjects;
using ::llvm::Triple;
using ::llvm::Twine;
using ::llvm::Type;
using ::llvm::TypeAtIndex;
using ::llvm::TypeSize;
using ::llvm::TypeSwitch;
using ::llvm::TypesAreDistinct;
using ::llvm::UTF16;
using ::llvm::UTF32;
using ::llvm::UTF8;
using ::llvm::UWTableKind;
using ::llvm::UniquePtrCast;
using ::llvm::UniqueStringSaver;
using ::llvm::Value;
using ::llvm::ValueFromPointerCast;
using ::llvm::ValueHandleBase;
using ::llvm::ValueIsPresent;
using ::llvm::ValueTypeFromRangeType;
using ::llvm::VectorLibrary;
using ::llvm::VectorType;
using ::llvm::VersionTuple;
using ::llvm::WinX64EHUnwindMode;
using ::llvm::WritableMemoryBuffer;
using ::llvm::WriteThroughMemoryBuffer;
using ::llvm::XXH128_hash_t;
using ::llvm::abs;
using ::llvm::accumulate;
using ::llvm::addEnumValues;
using ::llvm::add_const_past_pointer;
using ::llvm::add_lvalue_reference_if_not_pointer;
using ::llvm::adjacent_find;
using ::llvm::adl_begin;
using ::llvm::adl_end;
using ::llvm::adl_rbegin;
using ::llvm::adl_rend;
using ::llvm::adl_size;
using ::llvm::adl_swap;
using ::llvm::alignAddr;
using ::llvm::alignDown;
using ::llvm::alignTo;
using ::llvm::alignToPowerOf2;
using ::llvm::all_equal;
using ::llvm::all_of;
using ::llvm::all_of_zip;
using ::llvm::all_types_equal;
using ::llvm::all_types_equal_v;
using ::llvm::allocate_buffer;
using ::llvm::any_of;
using ::llvm::append_range;
using ::llvm::append_values;
using ::llvm::are_base_of;
using ::llvm::arrayRefFromStringRef;
using ::llvm::array_pod_sort;
using ::llvm::array_pod_sort_comparator;
using ::llvm::assumeAligned;
using ::llvm::binary_search;
using ::llvm::bind_back;
using ::llvm::bind_front;
using ::llvm::bit_cast;
using ::llvm::bit_ceil;
using ::llvm::bit_ceil_constexpr;
using ::llvm::bit_floor;
using ::llvm::bit_width;
using ::llvm::bit_width_constexpr;
using ::llvm::buffer_ostream;
using ::llvm::buffer_unique_ostream;
using ::llvm::byteswap;
using ::llvm::call_once;
using ::llvm::cannotOrderStrictlyGreater;
using ::llvm::cannotOrderStrictlyGreaterEq;
using ::llvm::cannotOrderStrictlyLess;
using ::llvm::cannotOrderStrictlyLessEq;
using ::llvm::cantFail;
using ::llvm::capacity_in_bytes;
using ::llvm::cast;
using ::llvm::cast_convert_val;
using ::llvm::cast_if_present;
using ::llvm::cast_or_null;
using ::llvm::cast_retty;
using ::llvm::cast_retty_impl;
using ::llvm::cast_retty_wrap;
using ::llvm::center_justify;
using ::llvm::checkNotNull;
using ::llvm::children;
using ::llvm::children_edges;
using ::llvm::commonAlignment;
using ::llvm::common_sint;
using ::llvm::common_uint;
using ::llvm::concat;
using ::llvm::concat_iterator;
using ::llvm::const_pointer_or_const_ref;
using ::llvm::const_set_bits_iterator_impl;
using ::llvm::consumeError;
using ::llvm::consumeSignedInteger;
using ::llvm::consumeUnsignedInteger;
using ::llvm::conversionOK;
using ::llvm::convertToCamelFromSnakeCase;
using ::llvm::convertToSnakeFromCamelCase;
using ::llvm::convertUTF16ToUTF8String;
using ::llvm::convertUTF32ToUTF8String;
using ::llvm::convertUTF8Sequence;
using ::llvm::convertUTF8ToUTF16String;
using ::llvm::convertWideToUTF8;
using ::llvm::copy;
using ::llvm::copy_if;
using ::llvm::count;
using ::llvm::count_if;
using ::llvm::countl_one;
using ::llvm::countl_zero;
using ::llvm::countl_zero_constexpr;
using ::llvm::countr_one;
using ::llvm::countr_zero;
using ::llvm::countr_zero_constexpr;
using ::llvm::createFileError;
using ::llvm::createStringError;
using ::llvm::dbgs;
using ::llvm::deallocate_buffer;
using ::llvm::decodeMaybeAlign;
using ::llvm::denormalModeKindName;
using ::llvm::deref;
using ::llvm::divideCeil;
using ::llvm::divideCeilSigned;
using ::llvm::divideFloorSigned;
using ::llvm::divideNearest;
using ::llvm::divideSignedWouldOverflow;
using ::llvm::drop_begin;
using ::llvm::drop_end;
using ::llvm::dyn_cast;
using ::llvm::dyn_cast_if_present;
using ::llvm::dyn_cast_or_null;
using ::llvm::early_inc_iterator_impl;
using ::llvm::enableif_int;
using ::llvm::encode;
using ::llvm::endianness;
using ::llvm::enum_iteration_traits;
using ::llvm::enum_seq;
using ::llvm::enum_seq_inclusive;
using ::llvm::enumerate;
using ::llvm::equal;
using ::llvm::equal_to;
using ::llvm::erase;
using ::llvm::erase_if;
using ::llvm::errc;
using ::llvm::errnoAsErrorCode;
using ::llvm::errorCodeToError;
using ::llvm::errorOrToExpected;
using ::llvm::errorToBool;
using ::llvm::errorToErrorCode;
using ::llvm::errs;
using ::llvm::exp;
using ::llvm::expectedToErrorOr;
using ::llvm::expectedToOptional;
using ::llvm::failed;
using ::llvm::failure;
using ::llvm::fatal_error_handler_t;
using ::llvm::fcAllFlags;
using ::llvm::fcFinite;
using ::llvm::fcInf;
using ::llvm::fcNan;
using ::llvm::fcNegFinite;
using ::llvm::fcNegInf;
using ::llvm::fcNegNormal;
using ::llvm::fcNegSubnormal;
using ::llvm::fcNegZero;
using ::llvm::fcNegative;
using ::llvm::fcNone;
using ::llvm::fcNormal;
using ::llvm::fcPosFinite;
using ::llvm::fcPosInf;
using ::llvm::fcPosNormal;
using ::llvm::fcPosSubnormal;
using ::llvm::fcPosZero;
using ::llvm::fcPositive;
using ::llvm::fcQNan;
using ::llvm::fcSNan;
using ::llvm::fcSubnormal;
using ::llvm::fcZero;
using ::llvm::fill;
using ::llvm::filter_iterator;
using ::llvm::filter_iterator_base;
using ::llvm::filter_iterator_impl;
using ::llvm::find;
using ::llvm::findMaximalSubpartOfIllFormedUTF8Sequence;
using ::llvm::find_if;
using ::llvm::find_if_not;
using ::llvm::find_singleton;
using ::llvm::find_singleton_nested;
using ::llvm::fltNanEncoding;
using ::llvm::fltNonfiniteBehavior;
using ::llvm::fltSemantics;
using ::llvm::fneg;
using ::llvm::for_each;
using ::llvm::force_iteration_on_noniterable_enum;
using ::llvm::force_iteration_on_noniterable_enum_t;
using ::llvm::format;
using ::llvm::format_bytes;
using ::llvm::format_bytes_with_ascii;
using ::llvm::format_decimal;
using ::llvm::format_hex;
using ::llvm::format_hex_no_prefix;
using ::llvm::format_object;
using ::llvm::format_provider;
using ::llvm::formatv;
using ::llvm::formatv_object;
using ::llvm::formatv_object_base;
using ::llvm::frexp;
using ::llvm::fromHex;
using ::llvm::from_range;
using ::llvm::from_range_t;
using ::llvm::function_ref;
using ::llvm::function_traits;
using ::llvm::get;
using ::llvm::getAllocToken;
using ::llvm::getAllocTokenModeAsString;
using ::llvm::getAllocTokenModeFromString;
using ::llvm::getAsSignedInteger;
using ::llvm::getAsUnsignedInteger;
using ::llvm::getAutoSenseRadix;
using ::llvm::getBugReportMsg;
using ::llvm::getDefaultPrecision;
using ::llvm::getKCFITypeID;
using ::llvm::getMergedAtomicOrdering;
using ::llvm::getNumBytesForUTF8;
using ::llvm::getOrdinalSuffix;
using ::llvm::getSingleElement;
using ::llvm::getToken;
using ::llvm::getUTF8SequenceSize;
using ::llvm::get_array_pod_sort_comparator;
using ::llvm::get_cpus;
using ::llvm::get_max_thread_name_length;
using ::llvm::get_physical_cores;
using ::llvm::get_thread_affinity_mask;
using ::llvm::get_thread_name;
using ::llvm::get_threadid;
using ::llvm::get_threadpool_strategy;
using ::llvm::handleAllErrors;
using ::llvm::handleErrorImpl;
using ::llvm::handleErrors;
using ::llvm::handleExpected;
using ::llvm::hardware_concurrency;
using ::llvm::hasNItems;
using ::llvm::hasNItemsOrLess;
using ::llvm::hasNItemsOrMore;
using ::llvm::hasSingleElement;
using ::llvm::hasUTF16ByteOrderMark;
using ::llvm::has_equality_comparison_v;
using ::llvm::has_single_bit;
using ::llvm::hash_code;
using ::llvm::hash_combine;
using ::llvm::hash_combine_range;
using ::llvm::hash_value;
using ::llvm::heavyweight_hardware_concurrency;
using ::llvm::hexDigitValue;
using ::llvm::hexFromNibbles;
using ::llvm::hexdigit;
using ::llvm::huge_valf;
using ::llvm::identity;
using ::llvm::ilogb;
using ::llvm::includes;
using ::llvm::inconvertibleErrorCode;
using ::llvm::indent;
using ::llvm::index_range;
using ::llvm::indexed_accessor_iterator;
using ::llvm::indexed_accessor_range;
using ::llvm::install_bad_alloc_error_handler;
using ::llvm::install_fatal_error_handler;
using ::llvm::install_out_of_memory_new_handler;
using ::llvm::interleave;
using ::llvm::interleaveComma;
using ::llvm::inverse_children;
using ::llvm::inverse_fabs;
using ::llvm::inverse_nodes;
using ::llvm::invoke;
using ::llvm::iota_range;
using ::llvm::isASCII;
using ::llvm::isAcquireOrStronger;
using ::llvm::isAddrAligned;
using ::llvm::isAligned;
using ::llvm::isAlnum;
using ::llvm::isAlpha;
using ::llvm::isAtLeastOrStrongerThan;
using ::llvm::isDigit;
using ::llvm::isHexDigit;
using ::llvm::isInt;
using ::llvm::isIntN;
using ::llvm::isLegalUTF8Sequence;
using ::llvm::isLegalUTF8String;
using ::llvm::isLower;
using ::llvm::isMask_32;
using ::llvm::isMask_64;
using ::llvm::isPowerOf2_32;
using ::llvm::isPowerOf2_64;
using ::llvm::isPrefixedHexStyle;
using ::llvm::isPrint;
using ::llvm::isPunct;
using ::llvm::isReleaseOrStronger;
using ::llvm::isShiftedInt;
using ::llvm::isShiftedMask_32;
using ::llvm::isShiftedMask_64;
using ::llvm::isShiftedUInt;
using ::llvm::isSpace;
using ::llvm::isStrongerThan;
using ::llvm::isStrongerThanMonotonic;
using ::llvm::isStrongerThanUnordered;
using ::llvm::isUInt;
using ::llvm::isUIntN;
using ::llvm::isUpper;
using ::llvm::isValidAtomicOrdering;
using ::llvm::isValidAtomicOrderingCABI;
using ::llvm::is_bitmask_enum;
using ::llvm::is_contained;
using ::llvm::is_detected;
using ::llvm::is_incomplete_v;
using ::llvm::is_integral_or_enum;
using ::llvm::is_one_of;
using ::llvm::is_simple_type;
using ::llvm::is_sorted;
using ::llvm::is_sorted_constexpr;
using ::llvm::isa;
using ::llvm::isa_and_nonnull;
using ::llvm::isa_and_present;
using ::llvm::isa_impl;
using ::llvm::isa_impl_cl;
using ::llvm::isa_impl_wrap;
using ::llvm::iterator_adaptor_base;
using ::llvm::iterator_facade_base;
using ::llvm::iterator_range;
using ::llvm::itostr;
using ::llvm::jobserver_concurrency;
using ::llvm::join;
using ::llvm::joinErrors;
using ::llvm::join_items;
using ::llvm::largest_bitmask_enum_bit;
using ::llvm::left_justify;
using ::llvm::lenientConversion;
using ::llvm::less_first;
using ::llvm::less_second;
using ::llvm::lfExactlyHalf;
using ::llvm::lfExactlyZero;
using ::llvm::lfLessThanHalf;
using ::llvm::lfMoreThanHalf;
using ::llvm::llvm_execute_on_thread_impl;
using ::llvm::llvm_is_multithreaded;
using ::llvm::llvm_shutdown;
using ::llvm::llvm_shutdown_obj;
using ::llvm::llvm_thread_detach_impl;
using ::llvm::llvm_thread_get_current_id_impl;
using ::llvm::llvm_thread_get_id_impl;
using ::llvm::llvm_thread_join_impl;
using ::llvm::llvm_unreachable_internal;
using ::llvm::logAllUnhandledErrors;
using ::llvm::lostFraction;
using ::llvm::lower_bound;
using ::llvm::makeIntrusiveRefCnt;
using ::llvm::makeVisitor;
using ::llvm::make_const_ptr;
using ::llvm::make_const_ref;
using ::llvm::make_early_inc_range;
using ::llvm::make_error;
using ::llvm::make_error_code;
using ::llvm::make_filter_range;
using ::llvm::make_first_range;
using ::llvm::make_pointee_range;
using ::llvm::make_pointer_range;
using ::llvm::make_range;
using ::llvm::make_scope_exit;
using ::llvm::make_second_range;
using ::llvm::map_iterator;
using ::llvm::map_range;
using ::llvm::mapped_iterator;
using ::llvm::mapped_iterator_base;
using ::llvm::maskLeadingOnes;
using ::llvm::maskLeadingZeros;
using ::llvm::maskTrailingOnes;
using ::llvm::maskTrailingZeros;
using ::llvm::maxIntN;
using ::llvm::maxUIntN;
using ::llvm::max_element;
using ::llvm::maximum;
using ::llvm::maximumnum;
using ::llvm::maxnum;
using ::llvm::minIntN;
using ::llvm::min_element;
using ::llvm::minimum;
using ::llvm::minimumnum;
using ::llvm::minnum;
using ::llvm::mismatch;
using ::llvm::mod;
using ::llvm::move;
using ::llvm::neg;
using ::llvm::nodes;
using ::llvm::none_of;
using ::llvm::not_equal_to;
using ::llvm::nulls;
using ::llvm::object_creator;
using ::llvm::object_deleter;
using ::llvm::offsetToAlignedAddr;
using ::llvm::offsetToAlignment;
using ::llvm::on_first;
using ::llvm::once_flag;
using ::llvm::operator!=;
using ::llvm::operator&;
using ::llvm::operator*;
using ::llvm::operator+;
using ::llvm::operator+=;
using ::llvm::operator-;
using ::llvm::operator<;
using ::llvm::operator<<;
using ::llvm::operator<=;
using ::llvm::operator==;
using ::llvm::operator>;
using ::llvm::operator>=;
using ::llvm::operator^;
using ::llvm::operator|;
using ::llvm::operator~;
using ::llvm::optimal_concurrency;
using ::llvm::orderedStrictlyGreater;
using ::llvm::orderedStrictlyLess;
using ::llvm::outs;
using ::llvm::pair_hash;
using ::llvm::parseDenormalFPAttribute;
using ::llvm::parseDenormalFPAttributeComponent;
using ::llvm::parseKCFIHashAlgorithm;
using ::llvm::partition;
using ::llvm::partition_point;
using ::llvm::pointee_iterator;
using ::llvm::pointer_iterator;
using ::llvm::popcount;
using ::llvm::printDebugLog;
using ::llvm::printEscapedString;
using ::llvm::printHTMLEscaped;
using ::llvm::printLowerCase;
using ::llvm::printPercentEncoded;
using ::llvm::product_of;
using ::llvm::range_size;
using ::llvm::rank;
using ::llvm::raw_fd_ostream;
using ::llvm::raw_fd_stream;
using ::llvm::raw_null_ostream;
using ::llvm::raw_ostream;
using ::llvm::raw_pointer_iterator;
using ::llvm::raw_pwrite_stream;
using ::llvm::raw_string_ostream;
using ::llvm::raw_svector_ostream;
using ::llvm::remove_bad_alloc_error_handler;
using ::llvm::remove_cvref;
using ::llvm::remove_cvref_t;
using ::llvm::remove_fatal_error_handler;
using ::llvm::remove_if;
using ::llvm::replace;
using ::llvm::replace_copy;
using ::llvm::replace_copy_if;
using ::llvm::reportFatalInternalError;
using ::llvm::reportFatalUsageError;
using ::llvm::report_bad_alloc_error;
using ::llvm::report_fatal_error;
using ::llvm::reverse;
using ::llvm::reverseBits;
using ::llvm::reverse_conditionally;
using ::llvm::right_justify;
using ::llvm::rotl;
using ::llvm::rotr;
using ::llvm::safe_calloc;
using ::llvm::safe_malloc;
using ::llvm::safe_realloc;
using ::llvm::scalbn;
using ::llvm::scope_exit;
using ::llvm::search;
using ::llvm::seq;
using ::llvm::seq_inclusive;
using ::llvm::setBugReportMsg;
using ::llvm::set_thread_name;
using ::llvm::set_thread_priority;
using ::llvm::shouldReverseIterate;
using ::llvm::shuffle;
using ::llvm::simplify_type;
using ::llvm::size;
using ::llvm::sort;
using ::llvm::sourceExhausted;
using ::llvm::sourceIllegal;
using ::llvm::spell;
using ::llvm::split;
using ::llvm::stable_sort;
using ::llvm::stack_float_t;
using ::llvm::strictConversion;
using ::llvm::stringifyKCFIHashAlgorithm;
using ::llvm::succeeded;
using ::llvm::success;
using ::llvm::sum_of;
using ::llvm::targetExhausted;
using ::llvm::thread;
using ::llvm::toCABI;
using ::llvm::toHex;
using ::llvm::toIRString;
using ::llvm::toLower;
using ::llvm::toString;
using ::llvm::toStringRef;
using ::llvm::toStringRefArray;
using ::llvm::toStringWithoutConsuming;
using ::llvm::toUpper;
using ::llvm::to_address;
using ::llvm::to_float;
using ::llvm::to_integer;
using ::llvm::to_string;
using ::llvm::to_underlying;
using ::llvm::to_vector;
using ::llvm::to_vector_of;
using ::llvm::transform;
using ::llvm::transformOptional;
using ::llvm::tryGetFromHex;
using ::llvm::tryGetHexFromNibbles;
using ::llvm::type_identity;
using ::llvm::type_identity_t;
using ::llvm::uninitialized_copy;
using ::llvm::unique;
using ::llvm::unique_dyn_cast;
using ::llvm::unique_dyn_cast_or_null;
using ::llvm::unique_function;
using ::llvm::unknown_sign;
using ::llvm::unwrap;
using ::llvm::upper_bound;
using ::llvm::utohexstr;
using ::llvm::utostr;
using ::llvm::visitErrors;
using ::llvm::wrap;
using ::llvm::writeToOutput;
using ::llvm::write_double;
using ::llvm::write_hex;
using ::llvm::write_integer;
using ::llvm::xxh3_128bits;
using ::llvm::xxh3_64bits;
using ::llvm::zip;
using ::llvm::zip_equal;
using ::llvm::zip_first;
using ::llvm::zip_longest;
}

export namespace llvm::APIntOps {
using ::llvm::APIntOps::GetMostSignificantDifferentBit;
using ::llvm::APIntOps::GreatestCommonDivisor;
using ::llvm::APIntOps::RoundAPIntToDouble;
using ::llvm::APIntOps::RoundAPIntToFloat;
using ::llvm::APIntOps::RoundDoubleToAPInt;
using ::llvm::APIntOps::RoundFloatToAPInt;
using ::llvm::APIntOps::RoundSignedAPIntToDouble;
using ::llvm::APIntOps::RoundSignedAPIntToFloat;
using ::llvm::APIntOps::RoundingSDiv;
using ::llvm::APIntOps::RoundingUDiv;
using ::llvm::APIntOps::ScaleBitMask;
using ::llvm::APIntOps::SolveQuadraticEquationWrap;
using ::llvm::APIntOps::abds;
using ::llvm::APIntOps::abdu;
using ::llvm::APIntOps::avgCeilS;
using ::llvm::APIntOps::avgCeilU;
using ::llvm::APIntOps::avgFloorS;
using ::llvm::APIntOps::avgFloorU;
using ::llvm::APIntOps::clmul;
using ::llvm::APIntOps::clmulh;
using ::llvm::APIntOps::clmulr;
using ::llvm::APIntOps::fshl;
using ::llvm::APIntOps::fshr;
using ::llvm::APIntOps::mulhs;
using ::llvm::APIntOps::mulhu;
using ::llvm::APIntOps::mulsExtended;
using ::llvm::APIntOps::muluExtended;
using ::llvm::APIntOps::pdep;
using ::llvm::APIntOps::pext;
using ::llvm::APIntOps::pow;
using ::llvm::APIntOps::smax;
using ::llvm::APIntOps::smin;
using ::llvm::APIntOps::umax;
using ::llvm::APIntOps::umin;
}

export namespace llvm::BitmaskEnumDetail {
using ::llvm::BitmaskEnumDetail::Mask;
using ::llvm::BitmaskEnumDetail::Underlying;
using ::llvm::BitmaskEnumDetail::any;
using ::llvm::BitmaskEnumDetail::operator!;
using ::llvm::BitmaskEnumDetail::operator&;
using ::llvm::BitmaskEnumDetail::operator&=;
using ::llvm::BitmaskEnumDetail::operator<<;
using ::llvm::BitmaskEnumDetail::operator<<=;
using ::llvm::BitmaskEnumDetail::operator>>;
using ::llvm::BitmaskEnumDetail::operator>>=;
using ::llvm::BitmaskEnumDetail::operator^;
using ::llvm::BitmaskEnumDetail::operator^=;
using ::llvm::BitmaskEnumDetail::operator|;
using ::llvm::BitmaskEnumDetail::operator|=;
using ::llvm::BitmaskEnumDetail::operator~;
}

export namespace llvm::CodeGenOpt {
using ::llvm::CodeGenOpt::getLevel;
using ::llvm::CodeGenOpt::parseLevel;
}

export namespace llvm::CodeModel {
using ::llvm::CodeModel::Kernel;
using ::llvm::CodeModel::Large;
using ::llvm::CodeModel::Medium;
using ::llvm::CodeModel::Model;
using ::llvm::CodeModel::Small;
using ::llvm::CodeModel::Tiny;
}

export namespace llvm::FPOpFusion {
using ::llvm::FPOpFusion::FPOpFusionMode;
using ::llvm::FPOpFusion::Fast;
using ::llvm::FPOpFusion::Standard;
using ::llvm::FPOpFusion::Strict;
}

export namespace llvm::FloatABI {
using ::llvm::FloatABI::ABIType;
using ::llvm::FloatABI::Default;
using ::llvm::FloatABI::Hard;
using ::llvm::FloatABI::Soft;
}

export namespace llvm::JumpTable {
using ::llvm::JumpTable::Arity;
using ::llvm::JumpTable::Full;
using ::llvm::JumpTable::JumpTableType;
using ::llvm::JumpTable::Simplified;
using ::llvm::JumpTable::Single;
}

export namespace llvm::PICLevel {
using ::llvm::PICLevel::BigPIC;
using ::llvm::PICLevel::Level;
using ::llvm::PICLevel::NotPIC;
using ::llvm::PICLevel::SmallPIC;
}

export namespace llvm::PIELevel {
using ::llvm::PIELevel::Default;
using ::llvm::PIELevel::Large;
using ::llvm::PIELevel::Level;
using ::llvm::PIELevel::Small;
}

export namespace llvm::Reloc {
using ::llvm::Reloc::DynamicNoPIC;
using ::llvm::Reloc::Model;
using ::llvm::Reloc::PIC_;
using ::llvm::Reloc::ROPI;
using ::llvm::Reloc::ROPI_RWPI;
using ::llvm::Reloc::RWPI;
using ::llvm::Reloc::Static;
}

export namespace llvm::TLSModel {
using ::llvm::TLSModel::GeneralDynamic;
using ::llvm::TLSModel::InitialExec;
using ::llvm::TLSModel::LocalDynamic;
using ::llvm::TLSModel::LocalExec;
using ::llvm::TLSModel::Model;
}

export namespace llvm::ThreadModel {
using ::llvm::ThreadModel::Model;
using ::llvm::ThreadModel::POSIX;
using ::llvm::ThreadModel::Single;
}

export namespace llvm::ZeroCallUsedRegs {
using ::llvm::ZeroCallUsedRegs::ZeroCallUsedRegsKind;
}

export namespace llvm::adl_detail {
using ::llvm::adl_detail::begin;
using ::llvm::adl_detail::begin_impl;
using ::llvm::adl_detail::end;
using ::llvm::adl_detail::end_impl;
using ::llvm::adl_detail::rbegin;
using ::llvm::adl_detail::rbegin_impl;
using ::llvm::adl_detail::rend;
using ::llvm::adl_detail::rend_impl;
using ::llvm::adl_detail::size;
using ::llvm::adl_detail::size_impl;
using ::llvm::adl_detail::swap;
using ::llvm::adl_detail::swap_impl;
}

export namespace llvm::bitc {
using ::llvm::bitc::BLOCKINFO_BLOCK_ID;
using ::llvm::bitc::BLOCKINFO_CODE_BLOCKNAME;
using ::llvm::bitc::BLOCKINFO_CODE_SETBID;
using ::llvm::bitc::BLOCKINFO_CODE_SETRECORDNAME;
using ::llvm::bitc::BlockIDWidth;
using ::llvm::bitc::BlockInfoCodes;
using ::llvm::bitc::BlockSizeWidth;
using ::llvm::bitc::CodeLenWidth;
using ::llvm::bitc::DEFINE_ABBREV;
using ::llvm::bitc::END_BLOCK;
using ::llvm::bitc::ENTER_SUBBLOCK;
using ::llvm::bitc::FIRST_APPLICATION_ABBREV;
using ::llvm::bitc::FIRST_APPLICATION_BLOCKID;
using ::llvm::bitc::FixedAbbrevIDs;
using ::llvm::bitc::StandardBlockIDs;
using ::llvm::bitc::StandardWidths;
using ::llvm::bitc::UNABBREV_RECORD;
}

export namespace llvm::callable_detail {
using ::llvm::callable_detail::Callable;
}

export namespace llvm::cl {
using ::llvm::cl::AddExtraVersionPrinter;
using ::llvm::cl::AddLiteralOption;
using ::llvm::cl::AlwaysPrefix;
using ::llvm::cl::CommaSeparated;
using ::llvm::cl::ConsumeAfter;
using ::llvm::cl::DefaultOption;
using ::llvm::cl::ExpandResponseFiles;
using ::llvm::cl::ExpansionContext;
using ::llvm::cl::FormattingFlags;
using ::llvm::cl::GenericOptionValue;
using ::llvm::cl::Grouping;
using ::llvm::cl::Hidden;
using ::llvm::cl::HideUnrelatedOptions;
using ::llvm::cl::LocationClass;
using ::llvm::cl::MiscFlags;
using ::llvm::cl::NormalFormatting;
using ::llvm::cl::NotHidden;
using ::llvm::cl::NumOccurrencesFlag;
using ::llvm::cl::OneOrMore;
using ::llvm::cl::Option;
using ::llvm::cl::OptionCategory;
using ::llvm::cl::OptionDiffPrinter;
using ::llvm::cl::OptionEnumValue;
using ::llvm::cl::OptionHidden;
using ::llvm::cl::OptionValue;
using ::llvm::cl::OptionValueCopy;
using ::llvm::cl::Optional;
using ::llvm::cl::ParseCommandLineOptions;
using ::llvm::cl::Positional;
using ::llvm::cl::PositionalEatsArgs;
using ::llvm::cl::Prefix;
using ::llvm::cl::PrintHelpMessage;
using ::llvm::cl::PrintOptionValues;
using ::llvm::cl::PrintVersionMessage;
using ::llvm::cl::ProvidePositionalOption;
using ::llvm::cl::ReallyHidden;
using ::llvm::cl::Required;
using ::llvm::cl::ResetAllOptionOccurrences;
using ::llvm::cl::ResetCommandLineParser;
using ::llvm::cl::SetVersionPrinter;
using ::llvm::cl::Sink;
using ::llvm::cl::SubCommand;
using ::llvm::cl::SubCommandGroup;
using ::llvm::cl::TokenizeGNUCommandLine;
using ::llvm::cl::TokenizeWindowsCommandLine;
using ::llvm::cl::TokenizeWindowsCommandLineFull;
using ::llvm::cl::TokenizeWindowsCommandLineNoCopy;
using ::llvm::cl::TokenizerCallback;
using ::llvm::cl::ValueDisallowed;
using ::llvm::cl::ValueExpected;
using ::llvm::cl::ValueOptional;
using ::llvm::cl::ValueRequired;
using ::llvm::cl::ValuesClass;
using ::llvm::cl::VersionPrinterTy;
using ::llvm::cl::ZeroOrMore;
using ::llvm::cl::alias;
using ::llvm::cl::aliasopt;
using ::llvm::cl::applicator;
using ::llvm::cl::apply;
using ::llvm::cl::basic_parser;
using ::llvm::cl::basic_parser_impl;
using ::llvm::cl::bits;
using ::llvm::cl::bits_storage;
using ::llvm::cl::boolOrDefault;
using ::llvm::cl::callback;
using ::llvm::cl::cat;
using ::llvm::cl::cb;
using ::llvm::cl::desc;
using ::llvm::cl::expandResponseFiles;
using ::llvm::cl::extrahelp;
using ::llvm::cl::generic_parser_base;
using ::llvm::cl::getCompilerBuildConfig;
using ::llvm::cl::getGeneralCategory;
using ::llvm::cl::getRegisteredOptions;
using ::llvm::cl::getRegisteredSubcommands;
using ::llvm::cl::init;
using ::llvm::cl::initializer;
using ::llvm::cl::list;
using ::llvm::cl::list_init;
using ::llvm::cl::list_initializer;
using ::llvm::cl::list_storage;
using ::llvm::cl::location;
using ::llvm::cl::multi_val;
using ::llvm::cl::opt;
using ::llvm::cl::opt_storage;
using ::llvm::cl::parser;
using ::llvm::cl::printBuildConfig;
using ::llvm::cl::printOptionDiff;
using ::llvm::cl::sub;
using ::llvm::cl::tokenizeConfigFile;
using ::llvm::cl::value_desc;
using ::llvm::cl::values;
}

export namespace llvm::cl::detail {
using ::llvm::cl::detail::callback_traits;
}

export namespace llvm::codegenoptions {
using ::llvm::codegenoptions::DIF_CodeView;
using ::llvm::codegenoptions::DIF_DWARF;
using ::llvm::codegenoptions::DebugDirectivesOnly;
using ::llvm::codegenoptions::DebugInfoConstructor;
using ::llvm::codegenoptions::DebugInfoFormat;
using ::llvm::codegenoptions::DebugInfoKind;
using ::llvm::codegenoptions::DebugLineTablesOnly;
using ::llvm::codegenoptions::DebugTemplateNamesKind;
using ::llvm::codegenoptions::FullDebugInfo;
using ::llvm::codegenoptions::LimitedDebugInfo;
using ::llvm::codegenoptions::LocTrackingOnly;
using ::llvm::codegenoptions::NoDebugInfo;
using ::llvm::codegenoptions::UnusedTypeInfo;
}

export namespace llvm::compression {
using ::llvm::compression::Format;
using ::llvm::compression::Params;
using ::llvm::compression::compress;
using ::llvm::compression::decompress;
using ::llvm::compression::formatFor;
using ::llvm::compression::getReasonIfUnsupported;
}

export namespace llvm::compression::zlib {
using ::llvm::compression::zlib::compress;
using ::llvm::compression::zlib::decompress;
using ::llvm::compression::zlib::isAvailable;
}

export namespace llvm::compression::zstd {
using ::llvm::compression::zstd::compress;
using ::llvm::compression::zstd::decompress;
using ::llvm::compression::zstd::isAvailable;
}

export namespace llvm::densemap::detail {
using ::llvm::densemap::detail::UsedT;
using ::llvm::densemap::detail::allocAlign;
using ::llvm::densemap::detail::allocBytes;
using ::llvm::densemap::detail::forEachUsed;
using ::llvm::densemap::detail::mix;
using ::llvm::densemap::detail::setUsed;
using ::llvm::densemap::detail::unsetUsed;
using ::llvm::densemap::detail::used;
using ::llvm::densemap::detail::usedWords;
}

export namespace llvm::detail {
using ::llvm::detail::AllocatorHolder;
using ::llvm::detail::BindStorage;
using ::llvm::detail::CastFunc;
using ::llvm::detail::CastIfPresentFunc;
using ::llvm::detail::CheckedInt;
using ::llvm::detail::ConstantFnTag;
using ::llvm::detail::DenseMapPair;
using ::llvm::detail::DenseSet;
using ::llvm::detail::DenseSetEmpty;
using ::llvm::detail::DenseSetImpl;
using ::llvm::detail::DenseSetPair;
using ::llvm::detail::DoubleAPFloat;
using ::llvm::detail::DynCastFunc;
using ::llvm::detail::DynCastIfPresentFunc;
using ::llvm::detail::EnableIfCallable;
using ::llvm::detail::EnableUnlessSameType;
using ::llvm::detail::EnumeratorTupleType;
using ::llvm::detail::ExponentType;
using ::llvm::detail::FnConstant;
using ::llvm::detail::FnHolder;
using ::llvm::detail::HasPointerLikeTypeTraits;
using ::llvm::detail::IEEEFloat;
using ::llvm::detail::IsPointerLike;
using ::llvm::detail::IsRegistryType;
using ::llvm::detail::IsaAndPresentCheckPredicate;
using ::llvm::detail::IsaCheckPredicate;
using ::llvm::detail::IterOfRange;
using ::llvm::detail::PunnedPointer;
using ::llvm::detail::RegistryLinkListDeclarationMarker;
using ::llvm::detail::RegistryLinkListStorage;
using ::llvm::detail::RuntimeFnTag;
using ::llvm::detail::SafeIntIterator;
using ::llvm::detail::SelfType;
using ::llvm::detail::SmallDenseSet;
using ::llvm::detail::StaticCastFunc;
using ::llvm::detail::TypeSwitchBase;
using ::llvm::detail::UniqueFunctionBase;
using ::llvm::detail::ValueOfRange;
using ::llvm::detail::Visitor;
using ::llvm::detail::ZipLongestItemType;
using ::llvm::detail::ZipLongestTupleType;
using ::llvm::detail::ZipTupleType;
using ::llvm::detail::ZippyIteratorTuple;
using ::llvm::detail::all_of_zip_predicate_first;
using ::llvm::detail::all_of_zip_predicate_last;
using ::llvm::detail::canTypeFitValue;
using ::llvm::detail::check_has_free_function_rbegin;
using ::llvm::detail::check_has_free_function_size;
using ::llvm::detail::check_has_member_contains_t;
using ::llvm::detail::check_has_member_find_t;
using ::llvm::detail::cmpResult;
using ::llvm::detail::combineHashValue;
using ::llvm::detail::compare_nullptr_t;
using ::llvm::detail::concat_range;
using ::llvm::detail::decay_if_c_char_array;
using ::llvm::detail::decay_if_c_char_array_t;
using ::llvm::detail::declval;
using ::llvm::detail::deref_or_none;
using ::llvm::detail::detector;
using ::llvm::detail::enumStringsStorageSize;
using ::llvm::detail::enumerator_result;
using ::llvm::detail::first_or_second_type;
using ::llvm::detail::fltCategory;
using ::llvm::detail::frexp;
using ::llvm::detail::fwd_or_bidi_tag;
using ::llvm::detail::getRegistryLinkListInstance;
using ::llvm::detail::has_equality_comparison;
using ::llvm::detail::has_number_t;
using ::llvm::detail::has_sizeof;
using ::llvm::detail::hash_value;
using ::llvm::detail::ilogb;
using ::llvm::detail::index_iterator;
using ::llvm::detail::index_stream;
using ::llvm::detail::indexed_accessor_range_base;
using ::llvm::detail::integerPart;
using ::llvm::detail::isPresent;
using ::llvm::detail::is_nullptr_comparable;
using ::llvm::detail::join_impl;
using ::llvm::detail::join_items_impl;
using ::llvm::detail::join_items_size;
using ::llvm::detail::join_one_item_size;
using ::llvm::detail::next_or_end;
using ::llvm::detail::opStatus;
using ::llvm::detail::operator!=;
using ::llvm::detail::operator==;
using ::llvm::detail::printBumpPtrAllocatorStats;
using ::llvm::detail::roundingMode;
using ::llvm::detail::scalbn;
using ::llvm::detail::sort_trivially_copyable;
using ::llvm::detail::to_float;
using ::llvm::detail::uninitializedTag;
using ::llvm::detail::unit;
using ::llvm::detail::unwrapValue;
using ::llvm::detail::zip_common;
using ::llvm::detail::zip_enumerator;
using ::llvm::detail::zip_first;
using ::llvm::detail::zip_longest_iterator;
using ::llvm::detail::zip_longest_range;
using ::llvm::detail::zip_shortest;
using ::llvm::detail::zip_traits;
using ::llvm::detail::zippy;
}

export namespace llvm::details {
using ::llvm::details::FixedOrScalableQuantity;
}

export namespace llvm::directive {
using ::llvm::directive::FindName;
using ::llvm::directive::Spelling;
using ::llvm::directive::VersionRange;
}

export namespace llvm::driver {
using ::llvm::driver::ProfileCSIRInstr;
using ::llvm::driver::ProfileClangInstr;
using ::llvm::driver::ProfileIRInstr;
using ::llvm::driver::ProfileIRSampleColdCov;
using ::llvm::driver::ProfileInstrKind;
using ::llvm::driver::ProfileNone;
using ::llvm::driver::VectorLibrary;
using ::llvm::driver::convertDriverVectorLibraryToVectorLibrary;
using ::llvm::driver::createTLII;
using ::llvm::driver::getDefaultProfileGenName;
}

export namespace llvm::dxbc {
using ::llvm::dxbc::AmplificationPSVInfo;
using ::llvm::dxbc::BitcodeHeader;
using ::llvm::dxbc::ComparisonFunc;
using ::llvm::dxbc::CompilerVersionFlags;
using ::llvm::dxbc::CompilerVersionHeader;
using ::llvm::dxbc::ContainerVersion;
using ::llvm::dxbc::D3DSystemValue;
using ::llvm::dxbc::DebugNameHeader;
using ::llvm::dxbc::DescriptorRangeFlags;
using ::llvm::dxbc::DomainPSVInfo;
using ::llvm::dxbc::FeatureFlags;
using ::llvm::dxbc::GeometryPSVInfo;
using ::llvm::dxbc::Hash;
using ::llvm::dxbc::HashFlags;
using ::llvm::dxbc::Header;
using ::llvm::dxbc::HullPSVInfo;
using ::llvm::dxbc::MeshPSVInfo;
using ::llvm::dxbc::PartHeader;
using ::llvm::dxbc::PartType;
using ::llvm::dxbc::PipelinePSVInfo;
using ::llvm::dxbc::PixelPSVInfo;
using ::llvm::dxbc::ProgramHeader;
using ::llvm::dxbc::ProgramSignatureElement;
using ::llvm::dxbc::ProgramSignatureHeader;
using ::llvm::dxbc::RootDescriptorFlags;
using ::llvm::dxbc::RootFlags;
using ::llvm::dxbc::RootParameterType;
using ::llvm::dxbc::RootSignatureVersion;
using ::llvm::dxbc::SamplerFilter;
using ::llvm::dxbc::ShaderHash;
using ::llvm::dxbc::ShaderVisibility;
using ::llvm::dxbc::SigComponentType;
using ::llvm::dxbc::SigMinPrecision;
using ::llvm::dxbc::StaticBorderColor;
using ::llvm::dxbc::StaticSamplerFlags;
using ::llvm::dxbc::TextureAddressMode;
using ::llvm::dxbc::VertexPSVInfo;
using ::llvm::dxbc::getComparisonFuncs;
using ::llvm::dxbc::getD3DSystemValues;
using ::llvm::dxbc::getDescriptorRangeFlags;
using ::llvm::dxbc::getProgramPartName;
using ::llvm::dxbc::getRootDescriptorFlags;
using ::llvm::dxbc::getRootFlags;
using ::llvm::dxbc::getRootParameterTypes;
using ::llvm::dxbc::getSamplerFilters;
using ::llvm::dxbc::getShaderStage;
using ::llvm::dxbc::getShaderVisibility;
using ::llvm::dxbc::getSigComponentTypes;
using ::llvm::dxbc::getSigMinPrecisions;
using ::llvm::dxbc::getStaticBorderColors;
using ::llvm::dxbc::getStaticSamplerFlags;
using ::llvm::dxbc::getTextureAddressModes;
using ::llvm::dxbc::isDebugProgramPart;
using ::llvm::dxbc::isProgramPart;
using ::llvm::dxbc::isValidAddress;
using ::llvm::dxbc::isValidBorderColor;
using ::llvm::dxbc::isValidComparisonFunc;
using ::llvm::dxbc::isValidCompilerVersionFlags;
using ::llvm::dxbc::isValidDescriptorRangeFlags;
using ::llvm::dxbc::isValidParameterType;
using ::llvm::dxbc::isValidRangeType;
using ::llvm::dxbc::isValidRootDesciptorFlags;
using ::llvm::dxbc::isValidSamplerFilter;
using ::llvm::dxbc::isValidShaderVisibility;
using ::llvm::dxbc::isValidStaticSamplerFlags;
using ::llvm::dxbc::parsePartType;
}

export namespace llvm::dxbc::PSV {
using ::llvm::dxbc::PSV::ComponentType;
using ::llvm::dxbc::PSV::InterpolationMode;
using ::llvm::dxbc::PSV::ResourceFlags;
using ::llvm::dxbc::PSV::ResourceKind;
using ::llvm::dxbc::PSV::ResourceType;
using ::llvm::dxbc::PSV::SemanticKind;
using ::llvm::dxbc::PSV::getComponentTypes;
using ::llvm::dxbc::PSV::getInterpolationModes;
using ::llvm::dxbc::PSV::getResourceKinds;
using ::llvm::dxbc::PSV::getResourceTypes;
using ::llvm::dxbc::PSV::getSemanticKinds;
}

export namespace llvm::dxbc::PSV::v0 {
using ::llvm::dxbc::PSV::v0::ResourceBindInfo;
using ::llvm::dxbc::PSV::v0::RuntimeInfo;
using ::llvm::dxbc::PSV::v0::SignatureElement;
}

export namespace llvm::dxbc::PSV::v1 {
using ::llvm::dxbc::PSV::v1::GeometryExtraInfo;
using ::llvm::dxbc::PSV::v1::MeshRuntimeInfo;
using ::llvm::dxbc::PSV::v1::RuntimeInfo;
}

export namespace llvm::dxbc::PSV::v2 {
using ::llvm::dxbc::PSV::v2::ResourceBindInfo;
using ::llvm::dxbc::PSV::v2::RuntimeInfo;
}

export namespace llvm::dxbc::PSV::v3 {
using ::llvm::dxbc::PSV::v3::RuntimeInfo;
}

export namespace llvm::dxbc::RTS0::v1 {
using ::llvm::dxbc::RTS0::v1::DescriptorRange;
using ::llvm::dxbc::RTS0::v1::RootConstants;
using ::llvm::dxbc::RTS0::v1::RootDescriptor;
using ::llvm::dxbc::RTS0::v1::RootParameterHeader;
using ::llvm::dxbc::RTS0::v1::RootSignatureHeader;
using ::llvm::dxbc::RTS0::v1::StaticSampler;
}

export namespace llvm::dxbc::RTS0::v2 {
using ::llvm::dxbc::RTS0::v2::DescriptorRange;
using ::llvm::dxbc::RTS0::v2::RootDescriptor;
}

export namespace llvm::dxbc::RTS0::v3 {
using ::llvm::dxbc::RTS0::v3::StaticSampler;
}

export namespace llvm::dxbc::SourceInfo {
using ::llvm::dxbc::SourceInfo::Header;
using ::llvm::dxbc::SourceInfo::SectionHeader;
using ::llvm::dxbc::SourceInfo::SectionType;
using ::llvm::dxbc::SourceInfo::getSectionName;
using ::llvm::dxbc::SourceInfo::getSectionTypes;
using ::llvm::dxbc::SourceInfo::isValidSectionType;
}

export namespace llvm::dxbc::SourceInfo::Args {
using ::llvm::dxbc::SourceInfo::Args::Header;
}

export namespace llvm::dxbc::SourceInfo::Contents {
using ::llvm::dxbc::SourceInfo::Contents::CompressionType;
using ::llvm::dxbc::SourceInfo::Contents::Entry;
using ::llvm::dxbc::SourceInfo::Contents::Header;
using ::llvm::dxbc::SourceInfo::Contents::getCompressionTypes;
using ::llvm::dxbc::SourceInfo::Contents::isValidCompressionType;
}

export namespace llvm::dxbc::SourceInfo::Names {
using ::llvm::dxbc::SourceInfo::Names::Entry;
using ::llvm::dxbc::SourceInfo::Names::HeaderOnDisk;
}

export namespace llvm::dxil {
using ::llvm::dxil::ElementType;
using ::llvm::dxil::ExtPropTags;
using ::llvm::dxil::ResourceClass;
using ::llvm::dxil::ResourceDimension;
using ::llvm::dxil::ResourceKind;
using ::llvm::dxil::SamplerFeedbackType;
using ::llvm::dxil::SamplerType;
using ::llvm::dxil::getResourceClassName;
}

export namespace llvm::hashbuilder_detail {
using ::llvm::hashbuilder_detail::HashCodeHashBuilder;
using ::llvm::hashbuilder_detail::HashCodeHasher;
using ::llvm::hashbuilder_detail::IsHashableData;
}

export namespace llvm::hashing::detail {
using ::llvm::hashing::detail::combine_bytes;
using ::llvm::hashing::detail::get_execution_seed;
using ::llvm::hashing::detail::get_hashable_data;
using ::llvm::hashing::detail::hash_combine_range_impl;
using ::llvm::hashing::detail::hash_integer_value;
using ::llvm::hashing::detail::is_hashable_data;
using ::llvm::hashing::detail::store_hashable_data;
using ::llvm::hashing::detail::total_hashable_size;
}

export namespace llvm::hlsl {
using ::llvm::hlsl::ResourceClass;
using ::llvm::hlsl::ResourceDimension;
}

export namespace llvm::hlsl::rootsig {
using ::llvm::hlsl::rootsig::DescriptorTable;
using ::llvm::hlsl::rootsig::DescriptorTableClause;
using ::llvm::hlsl::rootsig::Register;
using ::llvm::hlsl::rootsig::RegisterType;
using ::llvm::hlsl::rootsig::RootConstants;
using ::llvm::hlsl::rootsig::RootDescriptor;
using ::llvm::hlsl::rootsig::RootElement;
using ::llvm::hlsl::rootsig::StaticSampler;
using ::llvm::hlsl::rootsig::dumpRootElements;
using ::llvm::hlsl::rootsig::operator<<;
}

export namespace llvm::json {
using ::llvm::json::Array;
using ::llvm::json::OStream;
using ::llvm::json::Object;
using ::llvm::json::ObjectKey;
using ::llvm::json::ObjectMapper;
using ::llvm::json::ParseError;
using ::llvm::json::Path;
using ::llvm::json::Value;
using ::llvm::json::fixUTF8;
using ::llvm::json::fromJSON;
using ::llvm::json::isUTF8;
using ::llvm::json::is_uint_64_bit_v;
using ::llvm::json::operator!=;
using ::llvm::json::operator<;
using ::llvm::json::operator<<;
using ::llvm::json::operator==;
using ::llvm::json::parse;
using ::llvm::json::sortedElements;
using ::llvm::json::toJSON;
}

export namespace llvm::numbers {
using ::llvm::numbers::e;
using ::llvm::numbers::e_v;
using ::llvm::numbers::ef;
using ::llvm::numbers::egamma;
using ::llvm::numbers::egamma_v;
using ::llvm::numbers::egammaf;
using ::llvm::numbers::inv_pi;
using ::llvm::numbers::inv_pi_v;
using ::llvm::numbers::inv_pif;
using ::llvm::numbers::inv_sqrt2;
using ::llvm::numbers::inv_sqrt2_v;
using ::llvm::numbers::inv_sqrt2f;
using ::llvm::numbers::inv_sqrt3;
using ::llvm::numbers::inv_sqrt3_v;
using ::llvm::numbers::inv_sqrt3f;
using ::llvm::numbers::inv_sqrtpi;
using ::llvm::numbers::inv_sqrtpi_v;
using ::llvm::numbers::inv_sqrtpif;
using ::llvm::numbers::ln10;
using ::llvm::numbers::ln10_v;
using ::llvm::numbers::ln10f;
using ::llvm::numbers::ln2;
using ::llvm::numbers::ln2_v;
using ::llvm::numbers::ln2f;
using ::llvm::numbers::log10e;
using ::llvm::numbers::log10e_v;
using ::llvm::numbers::log10ef;
using ::llvm::numbers::log2e;
using ::llvm::numbers::log2e_v;
using ::llvm::numbers::log2ef;
using ::llvm::numbers::phi;
using ::llvm::numbers::phi_v;
using ::llvm::numbers::phif;
using ::llvm::numbers::pi;
using ::llvm::numbers::pi_v;
using ::llvm::numbers::pif;
using ::llvm::numbers::sqrt2;
using ::llvm::numbers::sqrt2_v;
using ::llvm::numbers::sqrt2f;
using ::llvm::numbers::sqrt3;
using ::llvm::numbers::sqrt3_v;
using ::llvm::numbers::sqrt3f;
using ::llvm::numbers::sqrtpi;
using ::llvm::numbers::sqrtpi_v;
using ::llvm::numbers::sqrtpif;
}

export namespace llvm::omp {
using ::llvm::omp::AddressSpace;
using ::llvm::omp::Association;
using ::llvm::omp::AssumptionClauseMappingInfo;
using ::llvm::omp::BindKind;
using ::llvm::omp::CancellationConstructType;
using ::llvm::omp::Category;
using ::llvm::omp::Clause;
using ::llvm::omp::DefaultKind;
using ::llvm::omp::Directive;
using ::llvm::omp::GV;
using ::llvm::omp::GrainsizeType;
using ::llvm::omp::ICVInitValue;
using ::llvm::omp::IdentFlag;
using ::llvm::omp::InternalControlVar;
using ::llvm::omp::LoopModifier;
using ::llvm::omp::MemoryOrderKind;
using ::llvm::omp::NumTasksType;
using ::llvm::omp::NumThreadsType;
using ::llvm::omp::OMPAtomicCompareOp;
using ::llvm::omp::OMPContext;
using ::llvm::omp::OMPDynGroupprivateFallbackType;
using ::llvm::omp::OMPInteropType;
using ::llvm::omp::OMPScheduleType;
using ::llvm::omp::OMPTgtExecModeFlags;
using ::llvm::omp::OMP_DEVICEID_UNDEF;
using ::llvm::omp::OMP_TGT_EXEC_MODE_BARE;
using ::llvm::omp::OMP_TGT_EXEC_MODE_GENERIC;
using ::llvm::omp::OMP_TGT_EXEC_MODE_GENERIC_SPMD;
using ::llvm::omp::OMP_TGT_EXEC_MODE_SPMD;
using ::llvm::omp::OMP_TGT_EXEC_MODE_SPMD_NO_LOOP;
using ::llvm::omp::OmpDefaultMapperName;
using ::llvm::omp::OpenMPOffloadMappingFlags;
using ::llvm::omp::OpenMPOffloadingReservedDeviceIDs;
using ::llvm::omp::OrderKind;
using ::llvm::omp::ProcBindKind;
using ::llvm::omp::RTLDependInfoFields;
using ::llvm::omp::RTLDependenceKindTy;
using ::llvm::omp::RuntimeFunction;
using ::llvm::omp::ScheduleKind;
using ::llvm::omp::SourceLanguage;
using ::llvm::omp::TraitProperty;
using ::llvm::omp::TraitSelector;
using ::llvm::omp::TraitSet;
using ::llvm::omp::VariantMatchInfo;
using ::llvm::omp::WorksharingLoopType;
using ::llvm::omp::deconstructOpenMPKernelName;
using ::llvm::omp::getAMDGPUGridValues;
using ::llvm::omp::getAllAssumeClauseOptions;
using ::llvm::omp::getBestVariantMatchForContext;
using ::llvm::omp::getBindKind;
using ::llvm::omp::getCancellationConstructType;
using ::llvm::omp::getCompoundConstruct;
using ::llvm::omp::getDirectiveAssociation;
using ::llvm::omp::getDirectiveCategory;
using ::llvm::omp::getDirectiveLanguages;
using ::llvm::omp::getGrainsizeType;
using ::llvm::omp::getLeafConstructs;
using ::llvm::omp::getLeafConstructsOrSelf;
using ::llvm::omp::getLeafOrCompositeConstructs;
using ::llvm::omp::getLoopModifierName;
using ::llvm::omp::getMaxLeafCount;
using ::llvm::omp::getMemoryOrderKind;
using ::llvm::omp::getNumTasksType;
using ::llvm::omp::getNumThreadsType;
using ::llvm::omp::getOpenMPBindKindName;
using ::llvm::omp::getOpenMPCancellationConstructTypeName;
using ::llvm::omp::getOpenMPClauseKind;
using ::llvm::omp::getOpenMPClauseKindAndVersions;
using ::llvm::omp::getOpenMPClauseName;
using ::llvm::omp::getOpenMPContextTraitPropertyForSelector;
using ::llvm::omp::getOpenMPContextTraitPropertyFullName;
using ::llvm::omp::getOpenMPContextTraitPropertyKind;
using ::llvm::omp::getOpenMPContextTraitPropertyName;
using ::llvm::omp::getOpenMPContextTraitSelectorForProperty;
using ::llvm::omp::getOpenMPContextTraitSelectorKind;
using ::llvm::omp::getOpenMPContextTraitSelectorName;
using ::llvm::omp::getOpenMPContextTraitSetForProperty;
using ::llvm::omp::getOpenMPContextTraitSetForSelector;
using ::llvm::omp::getOpenMPContextTraitSetKind;
using ::llvm::omp::getOpenMPContextTraitSetName;
using ::llvm::omp::getOpenMPDirectiveKind;
using ::llvm::omp::getOpenMPDirectiveKindAndVersions;
using ::llvm::omp::getOpenMPDirectiveName;
using ::llvm::omp::getOpenMPGrainsizeTypeName;
using ::llvm::omp::getOpenMPMemoryOrderKindName;
using ::llvm::omp::getOpenMPNumTasksTypeName;
using ::llvm::omp::getOpenMPNumThreadsTypeName;
using ::llvm::omp::getOpenMPOrderKindName;
using ::llvm::omp::getOpenMPProcBindKindName;
using ::llvm::omp::getOpenMPScheduleKindName;
using ::llvm::omp::getOpenMPVersions;
using ::llvm::omp::getOrderKind;
using ::llvm::omp::getProcBindKind;
using ::llvm::omp::getReservedLocatorNames;
using ::llvm::omp::getScheduleKind;
using ::llvm::omp::isAllowedClauseForDirective;
using ::llvm::omp::isAllowedLoopModifier;
using ::llvm::omp::isCombinedConstruct;
using ::llvm::omp::isCompositeConstruct;
using ::llvm::omp::isLeafConstruct;
using ::llvm::omp::isPrivatizingConstruct;
using ::llvm::omp::isValidTraitPropertyForTraitSetAndSelector;
using ::llvm::omp::isValidTraitSelectorForTraitSet;
using ::llvm::omp::isVariantApplicableInContext;
using ::llvm::omp::listOpenMPContextTraitProperties;
using ::llvm::omp::listOpenMPContextTraitSelectors;
using ::llvm::omp::listOpenMPContextTraitSets;
using ::llvm::omp::prettifyFunctionName;
}

export namespace llvm::opt {
using ::llvm::opt::Arg;
using ::llvm::opt::ArgList;
using ::llvm::opt::ArgStringList;
using ::llvm::opt::DefaultVis;
using ::llvm::opt::DerivedArgList;
using ::llvm::opt::DriverFlag;
using ::llvm::opt::DriverVisibility;
using ::llvm::opt::GenericOptTable;
using ::llvm::opt::HelpHidden;
using ::llvm::opt::InputArgList;
using ::llvm::opt::OptSpecifier;
using ::llvm::opt::OptTable;
using ::llvm::opt::Option;
using ::llvm::opt::PrecomputedOptTable;
using ::llvm::opt::RenderAsInput;
using ::llvm::opt::RenderJoined;
using ::llvm::opt::RenderSeparate;
using ::llvm::opt::Visibility;
using ::llvm::opt::arg_iterator;
}

export namespace llvm::pointer_union_detail {
using ::llvm::pointer_union_detail::PointerUnionMembers;
using ::llvm::pointer_union_detail::TagEntry;
using ::llvm::pointer_union_detail::bitsRequired;
using ::llvm::pointer_union_detail::computeExtendedTags;
using ::llvm::pointer_union_detail::computeFixedTags;
using ::llvm::pointer_union_detail::lowBitsAvailable;
using ::llvm::pointer_union_detail::typesInNonDecreasingBitOrder;
using ::llvm::pointer_union_detail::useFixedWidthTags;
}

export namespace llvm::support {
using ::llvm::support::aligned;
using ::llvm::support::aligned_big16_t;
using ::llvm::support::aligned_big32_t;
using ::llvm::support::aligned_big64_t;
using ::llvm::support::aligned_big_t;
using ::llvm::support::aligned_little16_t;
using ::llvm::support::aligned_little32_t;
using ::llvm::support::aligned_little64_t;
using ::llvm::support::aligned_little_t;
using ::llvm::support::aligned_ubig16_t;
using ::llvm::support::aligned_ubig32_t;
using ::llvm::support::aligned_ubig64_t;
using ::llvm::support::aligned_ulittle16_t;
using ::llvm::support::aligned_ulittle32_t;
using ::llvm::support::aligned_ulittle64_t;
using ::llvm::support::big16_t;
using ::llvm::support::big32_t;
using ::llvm::support::big64_t;
using ::llvm::support::big_t;
using ::llvm::support::little16_t;
using ::llvm::support::little32_t;
using ::llvm::support::little64_t;
using ::llvm::support::little_t;
using ::llvm::support::ubig16_t;
using ::llvm::support::ubig32_t;
using ::llvm::support::ubig64_t;
using ::llvm::support::ulittle16_t;
using ::llvm::support::ulittle32_t;
using ::llvm::support::ulittle64_t;
using ::llvm::support::ulittle8_t;
using ::llvm::support::unaligned;
using ::llvm::support::unaligned_int16_t;
using ::llvm::support::unaligned_int32_t;
using ::llvm::support::unaligned_int64_t;
using ::llvm::support::unaligned_uint16_t;
using ::llvm::support::unaligned_uint32_t;
using ::llvm::support::unaligned_uint64_t;
}

export namespace llvm::support::detail {
using ::llvm::support::detail::FormatFunctor;
using ::llvm::support::detail::FormatFunctorRef;
using ::llvm::support::detail::HelperFunctions;
using ::llvm::support::detail::PickAlignment;
using ::llvm::support::detail::is_cstring;
using ::llvm::support::detail::packed_endian_specific_integral;
using ::llvm::support::detail::use_char_formatter;
using ::llvm::support::detail::use_double_formatter;
using ::llvm::support::detail::use_integral_formatter;
using ::llvm::support::detail::use_pointer_formatter;
using ::llvm::support::detail::use_string_formatter;
}

export namespace llvm::support::endian {
using ::llvm::support::endian::byte_swap;
using ::llvm::support::endian::make_unsigned_t;
using ::llvm::support::endian::read16;
using ::llvm::support::endian::read16be;
using ::llvm::support::endian::read16le;
using ::llvm::support::endian::read32;
using ::llvm::support::endian::read32be;
using ::llvm::support::endian::read32le;
using ::llvm::support::endian::read64;
using ::llvm::support::endian::read64be;
using ::llvm::support::endian::read64le;
using ::llvm::support::endian::read;
using ::llvm::support::endian::readAtBitAlignment;
using ::llvm::support::endian::readNext;
using ::llvm::support::endian::write16;
using ::llvm::support::endian::write16be;
using ::llvm::support::endian::write16le;
using ::llvm::support::endian::write32;
using ::llvm::support::endian::write32be;
using ::llvm::support::endian::write32le;
using ::llvm::support::endian::write64;
using ::llvm::support::endian::write64be;
using ::llvm::support::endian::write64le;
using ::llvm::support::endian::write;
using ::llvm::support::endian::writeAtBitAlignment;
using ::llvm::support::endian::writeNext;
}

export namespace llvm::sys {
using ::llvm::sys::AddSignalHandler;
using ::llvm::sys::ChangeStdinMode;
using ::llvm::sys::ChangeStdinToBinary;
using ::llvm::sys::ChangeStdoutMode;
using ::llvm::sys::ChangeStdoutToBinary;
using ::llvm::sys::CleanupOnSignal;
using ::llvm::sys::DefaultOneShotPipeSignalHandler;
using ::llvm::sys::DisableSystemDialogsOnCrash;
using ::llvm::sys::DontRemoveFileOnSignal;
using ::llvm::sys::DynamicLibrary;
using ::llvm::sys::ExecuteAndWait;
using ::llvm::sys::ExecuteNoWait;
using ::llvm::sys::Mutex;
using ::llvm::sys::PrintStackTrace;
using ::llvm::sys::PrintStackTraceOnErrorSignal;
using ::llvm::sys::Process;
using ::llvm::sys::ProcessInfo;
using ::llvm::sys::ProcessStatistics;
using ::llvm::sys::RemoveFileOnSignal;
using ::llvm::sys::RunInterruptHandlers;
using ::llvm::sys::RunSignalHandlers;
using ::llvm::sys::ScopedLock;
using ::llvm::sys::SetInfoSignalFunction;
using ::llvm::sys::SetInterruptFunction;
using ::llvm::sys::SetOneShotPipeSignalFunction;
using ::llvm::sys::SignalHandlerCallback;
using ::llvm::sys::SmartMutex;
using ::llvm::sys::SmartScopedLock;
using ::llvm::sys::TimePoint;
using ::llvm::sys::UtcClock;
using ::llvm::sys::UtcTime;
using ::llvm::sys::WEM_CurrentCodePage;
using ::llvm::sys::WEM_UTF16;
using ::llvm::sys::WEM_UTF8;
using ::llvm::sys::Wait;
using ::llvm::sys::WindowsEncodingMethod;
using ::llvm::sys::commandLineFitsWithinSystemLimits;
using ::llvm::sys::findProgramByName;
using ::llvm::sys::getDefaultTargetTriple;
using ::llvm::sys::getHostCPUFeatures;
using ::llvm::sys::getHostCPUName;
using ::llvm::sys::getProcessTriple;
using ::llvm::sys::getSwappedBytes;
using ::llvm::sys::printArg;
using ::llvm::sys::printDefaultTargetAndDetectedCPU;
using ::llvm::sys::process_t;
using ::llvm::sys::procid_t;
using ::llvm::sys::swapByteOrder;
using ::llvm::sys::toTimePoint;
using ::llvm::sys::toTimeT;
using ::llvm::sys::toUtcTime;
using ::llvm::sys::unregisterHandlers;
using ::llvm::sys::writeFileWithEncoding;
}

export namespace llvm::sys::detail {
using ::llvm::sys::detail::getHostCPUNameForARM;
using ::llvm::sys::detail::getHostCPUNameForBPF;
using ::llvm::sys::detail::getHostCPUNameForPowerPC;
using ::llvm::sys::detail::getHostCPUNameForRISCV;
using ::llvm::sys::detail::getHostCPUNameForS390x;
using ::llvm::sys::detail::getHostCPUNameForSPARC;
}

export namespace llvm::sys::detail::x86 {
using ::llvm::sys::detail::x86::VendorSignatures;
using ::llvm::sys::detail::x86::getVendorSignature;
}

export namespace llvm::sys::fs {
using ::llvm::sys::fs::AccessMode;
using ::llvm::sys::fs::CD_CreateAlways;
using ::llvm::sys::fs::CD_CreateNew;
using ::llvm::sys::fs::CD_OpenAlways;
using ::llvm::sys::fs::CD_OpenExisting;
using ::llvm::sys::fs::CreationDisposition;
using ::llvm::sys::fs::DefaultReadChunkSize;
using ::llvm::sys::fs::FA_Read;
using ::llvm::sys::fs::FA_Write;
using ::llvm::sys::fs::FileAccess;
using ::llvm::sys::fs::FileLocker;
using ::llvm::sys::fs::LockKind;
using ::llvm::sys::fs::OF_Append;
using ::llvm::sys::fs::OF_CRLF;
using ::llvm::sys::fs::OF_ChildInherit;
using ::llvm::sys::fs::OF_Delete;
using ::llvm::sys::fs::OF_None;
using ::llvm::sys::fs::OF_OpenDirectory;
using ::llvm::sys::fs::OF_Text;
using ::llvm::sys::fs::OF_TextWithCRLF;
using ::llvm::sys::fs::OF_UpdateAtime;
using ::llvm::sys::fs::OF_UpdateAttributes;
using ::llvm::sys::fs::OpenFlags;
using ::llvm::sys::fs::TempFile;
using ::llvm::sys::fs::UniqueID;
using ::llvm::sys::fs::access;
using ::llvm::sys::fs::all_all;
using ::llvm::sys::fs::all_exe;
using ::llvm::sys::fs::all_perms;
using ::llvm::sys::fs::all_read;
using ::llvm::sys::fs::all_write;
using ::llvm::sys::fs::basic_file_status;
using ::llvm::sys::fs::can_execute;
using ::llvm::sys::fs::can_write;
using ::llvm::sys::fs::changeFileOwnership;
using ::llvm::sys::fs::closeFile;
using ::llvm::sys::fs::convertFDToNativeFile;
using ::llvm::sys::fs::copy_file;
using ::llvm::sys::fs::createTemporaryFile;
using ::llvm::sys::fs::createUniqueDirectory;
using ::llvm::sys::fs::createUniqueFile;
using ::llvm::sys::fs::createUniquePath;
using ::llvm::sys::fs::create_directories;
using ::llvm::sys::fs::create_directory;
using ::llvm::sys::fs::create_hard_link;
using ::llvm::sys::fs::create_link;
using ::llvm::sys::fs::create_symlink;
using ::llvm::sys::fs::current_path;
using ::llvm::sys::fs::directory_entry;
using ::llvm::sys::fs::directory_iterator;
using ::llvm::sys::fs::disk_space;
using ::llvm::sys::fs::equivalent;
using ::llvm::sys::fs::exists;
using ::llvm::sys::fs::expand_tilde;
using ::llvm::sys::fs::file_size;
using ::llvm::sys::fs::file_status;
using ::llvm::sys::fs::file_t;
using ::llvm::sys::fs::file_type;
using ::llvm::sys::fs::getMainExecutable;
using ::llvm::sys::fs::getPermissions;
using ::llvm::sys::fs::getPotentiallyUniqueFileName;
using ::llvm::sys::fs::getPotentiallyUniqueTempFileName;
using ::llvm::sys::fs::getStderrHandle;
using ::llvm::sys::fs::getStdinHandle;
using ::llvm::sys::fs::getStdoutHandle;
using ::llvm::sys::fs::getUmask;
using ::llvm::sys::fs::getUniqueID;
using ::llvm::sys::fs::get_file_type;
using ::llvm::sys::fs::group_all;
using ::llvm::sys::fs::group_exe;
using ::llvm::sys::fs::group_read;
using ::llvm::sys::fs::group_write;
using ::llvm::sys::fs::is_directory;
using ::llvm::sys::fs::is_local;
using ::llvm::sys::fs::is_other;
using ::llvm::sys::fs::is_regular_file;
using ::llvm::sys::fs::is_symlink_file;
using ::llvm::sys::fs::kInvalidFile;
using ::llvm::sys::fs::lockFile;
using ::llvm::sys::fs::make_absolute;
using ::llvm::sys::fs::mapped_file_region;
using ::llvm::sys::fs::md5_contents;
using ::llvm::sys::fs::no_perms;
using ::llvm::sys::fs::openFile;
using ::llvm::sys::fs::openFileForRead;
using ::llvm::sys::fs::openFileForReadWrite;
using ::llvm::sys::fs::openFileForWrite;
using ::llvm::sys::fs::openNativeFile;
using ::llvm::sys::fs::openNativeFileForRead;
using ::llvm::sys::fs::openNativeFileForReadWrite;
using ::llvm::sys::fs::openNativeFileForWrite;
using ::llvm::sys::fs::operator&;
using ::llvm::sys::fs::operator&=;
using ::llvm::sys::fs::operator|;
using ::llvm::sys::fs::operator|=;
using ::llvm::sys::fs::operator~;
using ::llvm::sys::fs::others_all;
using ::llvm::sys::fs::others_exe;
using ::llvm::sys::fs::others_read;
using ::llvm::sys::fs::others_write;
using ::llvm::sys::fs::owner_all;
using ::llvm::sys::fs::owner_exe;
using ::llvm::sys::fs::owner_read;
using ::llvm::sys::fs::owner_write;
using ::llvm::sys::fs::perms;
using ::llvm::sys::fs::perms_not_known;
using ::llvm::sys::fs::readNativeFile;
using ::llvm::sys::fs::readNativeFileSlice;
using ::llvm::sys::fs::readNativeFileToEOF;
using ::llvm::sys::fs::readlink;
using ::llvm::sys::fs::real_path;
using ::llvm::sys::fs::recursive_directory_iterator;
using ::llvm::sys::fs::remove;
using ::llvm::sys::fs::remove_directories;
using ::llvm::sys::fs::rename;
using ::llvm::sys::fs::resize_file;
using ::llvm::sys::fs::resize_file_before_mapping_readwrite;
using ::llvm::sys::fs::resize_file_sparse;
using ::llvm::sys::fs::setLastAccessAndModificationTime;
using ::llvm::sys::fs::setPermissions;
using ::llvm::sys::fs::set_current_path;
using ::llvm::sys::fs::set_gid_on_exe;
using ::llvm::sys::fs::set_uid_on_exe;
using ::llvm::sys::fs::space_info;
using ::llvm::sys::fs::status;
using ::llvm::sys::fs::status_known;
using ::llvm::sys::fs::sticky_bit;
using ::llvm::sys::fs::tryLockFile;
using ::llvm::sys::fs::unlockFile;
}

export namespace llvm::sys::fs::detail {
using ::llvm::sys::fs::detail::DirIterState;
using ::llvm::sys::fs::detail::RecDirIterState;
using ::llvm::sys::fs::detail::directory_iterator_construct;
using ::llvm::sys::fs::detail::directory_iterator_destruct;
using ::llvm::sys::fs::detail::directory_iterator_increment;
}

export namespace llvm::sys::path {
using ::llvm::sys::path::Style;
using ::llvm::sys::path::append;
using ::llvm::sys::path::begin;
using ::llvm::sys::path::cache_directory;
using ::llvm::sys::path::const_iterator;
using ::llvm::sys::path::convert_to_slash;
using ::llvm::sys::path::end;
using ::llvm::sys::path::extension;
using ::llvm::sys::path::filename;
using ::llvm::sys::path::get_separator;
using ::llvm::sys::path::has_extension;
using ::llvm::sys::path::has_filename;
using ::llvm::sys::path::has_parent_path;
using ::llvm::sys::path::has_relative_path;
using ::llvm::sys::path::has_root_directory;
using ::llvm::sys::path::has_root_name;
using ::llvm::sys::path::has_root_path;
using ::llvm::sys::path::has_stem;
using ::llvm::sys::path::home_directory;
using ::llvm::sys::path::is_absolute;
using ::llvm::sys::path::is_absolute_gnu;
using ::llvm::sys::path::is_relative;
using ::llvm::sys::path::is_separator;
using ::llvm::sys::path::is_style_posix;
using ::llvm::sys::path::is_style_windows;
using ::llvm::sys::path::make_absolute;
using ::llvm::sys::path::make_preferred;
using ::llvm::sys::path::native;
using ::llvm::sys::path::parent_path;
using ::llvm::sys::path::rbegin;
using ::llvm::sys::path::relative_path;
using ::llvm::sys::path::remove_dots;
using ::llvm::sys::path::remove_filename;
using ::llvm::sys::path::remove_leading_dotslash;
using ::llvm::sys::path::rend;
using ::llvm::sys::path::replace_extension;
using ::llvm::sys::path::replace_path_prefix;
using ::llvm::sys::path::reverse_iterator;
using ::llvm::sys::path::root_directory;
using ::llvm::sys::path::root_name;
using ::llvm::sys::path::root_path;
using ::llvm::sys::path::stem;
using ::llvm::sys::path::system_temp_directory;
using ::llvm::sys::path::user_config_directory;
}

export namespace llvm::this_thread {
using ::llvm::this_thread::get_id;
}

export namespace llvm::trailing_objects_internal {
using ::llvm::trailing_objects_internal::ExtractSecondType;
using ::llvm::trailing_objects_internal::MaxAlignment;
using ::llvm::trailing_objects_internal::TrailingObjectsBase;
using ::llvm::trailing_objects_internal::TrailingObjectsImpl;
}

export namespace llvm::vfs {
using ::llvm::vfs::AtomicTracingFileSystem;
using ::llvm::vfs::File;
using ::llvm::vfs::FileSystem;
using ::llvm::vfs::InMemoryFileSystem;
using ::llvm::vfs::NullOutputFileImpl;
using ::llvm::vfs::OutputBackend;
using ::llvm::vfs::OutputConfig;
using ::llvm::vfs::OutputFile;
using ::llvm::vfs::OutputFileImpl;
using ::llvm::vfs::OverlayFileSystem;
using ::llvm::vfs::ProxyFileSystem;
using ::llvm::vfs::RedirectingFSDirIterImpl;
using ::llvm::vfs::RedirectingFileSystem;
using ::llvm::vfs::RedirectingFileSystemParser;
using ::llvm::vfs::Status;
using ::llvm::vfs::TracingFileSystem;
using ::llvm::vfs::TracingFileSystemImpl;
using ::llvm::vfs::YAMLVFSEntry;
using ::llvm::vfs::YAMLVFSWriter;
using ::llvm::vfs::collectVFSEntries;
using ::llvm::vfs::consumeDiscardOnDestroy;
using ::llvm::vfs::createPhysicalFileSystem;
using ::llvm::vfs::directory_entry;
using ::llvm::vfs::directory_iterator;
using ::llvm::vfs::getNextVirtualUniqueID;
using ::llvm::vfs::getRealFileSystem;
using ::llvm::vfs::getVFSFromYAML;
using ::llvm::vfs::recursive_directory_iterator;
}

export namespace llvm::vfs::detail {
using ::llvm::vfs::detail::DirIterImpl;
using ::llvm::vfs::detail::EmptyBaseClass;
using ::llvm::vfs::detail::InMemoryDirectory;
using ::llvm::vfs::detail::InMemoryNode;
using ::llvm::vfs::detail::NamedNodeOrError;
using ::llvm::vfs::detail::NewInMemoryNodeInfo;
using ::llvm::vfs::detail::RecDirIterState;
}

export namespace llvm::yaml {
using ::llvm::yaml::AliasNode;
using ::llvm::yaml::BlockScalarNode;
using ::llvm::yaml::BlockScalarTraits;
using ::llvm::yaml::CheckIsBool;
using ::llvm::yaml::CustomMappingTraits;
using ::llvm::yaml::Document;
using ::llvm::yaml::DocumentListTraits;
using ::llvm::yaml::EmptyContext;
using ::llvm::yaml::Hex16;
using ::llvm::yaml::Hex32;
using ::llvm::yaml::Hex64;
using ::llvm::yaml::Hex8;
using ::llvm::yaml::IO;
using ::llvm::yaml::Input;
using ::llvm::yaml::IsFlowSequenceBase;
using ::llvm::yaml::IsResizableBase;
using ::llvm::yaml::KeyValueNode;
using ::llvm::yaml::MappingContextTraits;
using ::llvm::yaml::MappingNode;
using ::llvm::yaml::MappingNormalization;
using ::llvm::yaml::MappingNormalizationHeap;
using ::llvm::yaml::MappingTraits;
using ::llvm::yaml::MissingTrait;
using ::llvm::yaml::Node;
using ::llvm::yaml::NodeKind;
using ::llvm::yaml::NullNode;
using ::llvm::yaml::Output;
using ::llvm::yaml::PolymorphicTraits;
using ::llvm::yaml::QuotingType;
using ::llvm::yaml::ScalarBitSetTraits;
using ::llvm::yaml::ScalarEnumerationTraits;
using ::llvm::yaml::ScalarNode;
using ::llvm::yaml::ScalarTraits;
using ::llvm::yaml::Scanner;
using ::llvm::yaml::SequenceElementTraits;
using ::llvm::yaml::SequenceNode;
using ::llvm::yaml::SequenceTraits;
using ::llvm::yaml::SequenceTraitsImpl;
using ::llvm::yaml::StdMapStringCustomMappingTraitsImpl;
using ::llvm::yaml::Stream;
using ::llvm::yaml::TaggedScalarTraits;
using ::llvm::yaml::Token;
using ::llvm::yaml::basic_collection_iterator;
using ::llvm::yaml::begin;
using ::llvm::yaml::check_resize_t;
using ::llvm::yaml::document_iterator;
using ::llvm::yaml::dumpTokens;
using ::llvm::yaml::escape;
using ::llvm::yaml::has_BlockScalarTraits;
using ::llvm::yaml::has_CustomMappingTraits;
using ::llvm::yaml::has_DocumentListTraits;
using ::llvm::yaml::has_FlowTraits;
using ::llvm::yaml::has_MappingEnumInputTraits;
using ::llvm::yaml::has_MappingTraits;
using ::llvm::yaml::has_MappingValidateTraits;
using ::llvm::yaml::has_PolymorphicTraits;
using ::llvm::yaml::has_ScalarBitSetTraits;
using ::llvm::yaml::has_ScalarEnumerationTraits;
using ::llvm::yaml::has_ScalarTraits;
using ::llvm::yaml::has_SequenceMethodTraits;
using ::llvm::yaml::has_SequenceTraits;
using ::llvm::yaml::has_TaggedScalarTraits;
using ::llvm::yaml::isBool;
using ::llvm::yaml::isNull;
using ::llvm::yaml::isNumeric;
using ::llvm::yaml::missingTraits;
using ::llvm::yaml::needsQuotes;
using ::llvm::yaml::operator<<;
using ::llvm::yaml::operator>>;
using ::llvm::yaml::parseBool;
using ::llvm::yaml::scanTokens;
using ::llvm::yaml::skip;
using ::llvm::yaml::unvalidatedMappingTraits;
using ::llvm::yaml::validatedMappingTraits;
using ::llvm::yaml::yamlize;
using ::llvm::yaml::yamlizeMappingEnumInput;
}

export namespace llvm::yaml::detail {
using ::llvm::yaml::detail::doMapping;
using ::llvm::yaml::detail::doValidate;
}

export namespace std {
using ::std::swap;
}

#if (defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)) || (defined(__linux__) && defined(__aarch64__)) || (defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)) || (defined(__APPLE__) && defined(__x86_64__))
export namespace llvm {
using ::llvm::DisableABIBreakingChecks;
using ::llvm::VerifyDisableABIBreakingChecks;
}
#endif

#if (defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)) || (defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG))
export namespace llvm {
using ::llvm::float128;
}
#endif

#if (defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)) || (defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG))
export namespace llvm {
using ::llvm::DebugStr;
using ::llvm::EnableABIBreakingChecks;
using ::llvm::VerifyEnableABIBreakingChecks;
using ::llvm::isCurrentDebugType;
using ::llvm::setCurrentDebugType;
using ::llvm::setCurrentDebugTypes;
}
#endif
