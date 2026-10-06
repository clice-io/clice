#pragma once
#define LLVM_MARK_AS_BITMASK_ENUM(LargestValue)                                  LLVM_BITMASK_LARGEST_ENUMERATOR = LargestValue
#define LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE()                                 using ::llvm::BitmaskEnumDetail::operator~;                                    using ::llvm::BitmaskEnumDetail::operator|;                                    using ::llvm::BitmaskEnumDetail::operator&;                                    using ::llvm::BitmaskEnumDetail::operator^;                                    using ::llvm::BitmaskEnumDetail::operator<<;                                   using ::llvm::BitmaskEnumDetail::operator>>;                                   using ::llvm::BitmaskEnumDetail::operator|=;                                   using ::llvm::BitmaskEnumDetail::operator&=;                                   using ::llvm::BitmaskEnumDetail::operator^=;                                   using ::llvm::BitmaskEnumDetail::operator<<=;                                  using ::llvm::BitmaskEnumDetail::operator>>=;                                  using ::llvm::BitmaskEnumDetail::operator!;                                    /* Force a semicolon at the end of this macro. */                              using ::llvm::BitmaskEnumDetail::any
#define LLVM_DEFAULT_TARGET_TRIPLE "aarch64-unknown-linux-gnu"
#define LLVM_ENABLE_THREADS 1
#define LLVM_ON_UNIX 1
#define LLVM_MAKE_OPT_ID_WITH_ID_PREFIX(                                           ID_PREFIX, PREFIXES_OFFSET, PREFIXED_NAME_OFFSET, ID, KIND, GROUP, ALIAS,      ALIASARGS, FLAGS, VISIBILITY, PARAM, HELPTEXT, HELPTEXTSFORVARIANTS,           METAVAR, VALUES, SUBCOMMANDIDS_OFFSET)                                       ID_PREFIX##ID
#define LLVM_MAKE_OPT_ID(PREFIXES_OFFSET, PREFIXED_NAME_OFFSET, ID, KIND,                               GROUP, ALIAS, ALIASARGS, FLAGS, VISIBILITY, PARAM,                             HELPTEXT, HELPTEXTSFORVARIANTS, METAVAR, VALUES,                               SUBCOMMANDIDS_OFFSET)                                   LLVM_MAKE_OPT_ID_WITH_ID_PREFIX(                                                   OPT_, PREFIXES_OFFSET, PREFIXED_NAME_OFFSET, ID, KIND, GROUP, ALIAS,           ALIASARGS, FLAGS, VISIBILITY, PARAM, HELPTEXT, HELPTEXTSFORVARIANTS,           METAVAR, VALUES, SUBCOMMANDIDS_OFFSET)
#define LLVM_ATTRIBUTE_VISIBILITY_DEFAULT __attribute__((visibility("default")))
#define LLVM_ABI
#define LLVM_READNONE __attribute__((__const__))
#define LLVM_READONLY __attribute__((__pure__))
#define LLVM_UNLIKELY(EXPR) __builtin_expect((bool)(EXPR), false)
#define LLVM_ATTRIBUTE_NOINLINE __attribute__((noinline))
#define LLVM_ATTRIBUTE_ALWAYS_INLINE inline __attribute__((always_inline))
#define LLVM_ATTRIBUTE_ALWAYS_INLINE_UNLESS_DEBUG LLVM_ATTRIBUTE_ALWAYS_INLINE
#define LLVM_ATTRIBUTE_NODEBUG __attribute__((nodebug))
#define LLVM_ATTRIBUTE_RETURNS_NONNULL __attribute__((returns_nonnull))
# define LLVM_BUILTIN_UNREACHABLE __builtin_unreachable()
# define LLVM_BUILTIN_TRAP __builtin_trap()
#define LLVM_DUMP_METHOD LLVM_ATTRIBUTE_NOINLINE
#define LLVM_PREFERRED_TYPE(T) __attribute__((preferred_type(T)))
    #define LLVM_DECLARE_VIRTUAL_ANCHOR_FUNCTION()                                  _Pragma("clang diagnostic push")                                              _Pragma("clang diagnostic ignored \"-Wunnecessary-virtual-specifier\"")       virtual void anchor()                                                         _Pragma("clang diagnostic pop")
#define llvm_unreachable(msg) LLVM_BUILTIN_UNREACHABLE
#define LLVM_YAML_IS_SEQUENCE_VECTOR_IMPL(TYPE, FLOW)                            namespace llvm {                                                               namespace yaml {                                                               static_assert(                                                                     !std::is_fundamental_v<TYPE> && !std::is_same_v<TYPE, std::string> &&              !std::is_same_v<TYPE, llvm::StringRef>,                                    "only use LLVM_YAML_IS_SEQUENCE_VECTOR for types you control");            template <> struct SequenceElementTraits<TYPE> {                                 static const bool flow = FLOW;                                               };                                                                             }                                                                              }
#define LLVM_YAML_IS_SEQUENCE_VECTOR(type)                                       LLVM_YAML_IS_SEQUENCE_VECTOR_IMPL(type, false)
