#pragma once
#define _MT
#  define _M_AMD64 100
#  define _M_X64 100
# define __MINGW_NAME_AW(func) func##A
#define assert(_Expression) ((void)0)
#define errno (*_errno())
#define EINVAL 22
#define ERANGE 34
#define _UI64_MAX 0xffffffffffffffffull
#define SIZE_MAX _UI64_MAX
#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5
#define alloca(x) __builtin_alloca((x))
#define ERANGE 34
#define _Ret_notnull_
#define _Post_writable_byte_size_(s)
#define SIGINT 2
#define SIGTERM 15
#define INT32_MAX 2147483647
#define INT64_MAX 9223372036854775807LL
#define UINT32_MAX 0xffffffffU  /* 4294967295U */
#define UINT64_MAX 0xffffffffffffffffULL /* 18446744073709551615ULL */
#define PTRDIFF_MAX INT64_MAX
#define SIZE_MAX UINT64_MAX
#define INT64_C(val) val##LL
#define UINT8_C(val) (val)
#define UINT64_C(val) val##ULL
#define SEEK_END 2
#define stderr (__acrt_iob_func(2))
#define errno (*_errno())
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
#define GetMessage __MINGW_NAME_AW(GetMessage)
#define FLT_EVAL_METHOD __FLT_EVAL_METHOD__
#define FLT_RADIX __FLT_RADIX__
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define offsetof(t, d) __builtin_offsetof(t, d)
#define _mm256_extract_epi64(X, N)   ((long long)__builtin_ia32_vec_ext_v4di((__v4di)(__m256i)(X), (int)(N)))
#define INT_MAX   __INT_MAX__
#define LONG_MAX  __LONG_MAX__
#define INT_MIN   (-__INT_MAX__  -1)
#define UINT_MAX  (__INT_MAX__  *2U +1U)
#define CHAR_BIT  __CHAR_BIT__
#define ULLONG_MAX (__LONG_LONG_MAX__*2ULL+1ULL)
