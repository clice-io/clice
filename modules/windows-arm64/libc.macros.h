#pragma once
#define _MT
#  define _M_ARM64 1
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
#define vcreate_u8(__p0) __extension__ ({   uint8x8_t __ret;   uint64_t __promote = __p0;   __ret = __builtin_bit_cast(uint8x8_t, __promote);   __ret; })
#define vextq_u8(__p0, __p1, __p2) __extension__ ({   uint8x16_t __ret;   uint8x16_t __s0 = __p0;   uint8x16_t __s1 = __p1;   __ret = __builtin_bit_cast(uint8x16_t, __builtin_neon_vextq_v(__builtin_bit_cast(int8x16_t, __s0), __builtin_bit_cast(int8x16_t, __s1), __p2, 48));   __ret; })
#define vextq_s8(__p0, __p1, __p2) __extension__ ({   int8x16_t __ret;   int8x16_t __s0 = __p0;   int8x16_t __s1 = __p1;   __ret = __builtin_bit_cast(int8x16_t, __builtin_neon_vextq_v(__builtin_bit_cast(int8x16_t, __s0), __builtin_bit_cast(int8x16_t, __s1), __p2, 32));   __ret; })
#define vgetq_lane_u64(__p0, __p1) __extension__ ({   uint64_t __ret;   uint64x2_t __s0 = __p0;   __ret = __builtin_bit_cast(uint64_t, __builtin_neon_vgetq_lane_i64(__builtin_bit_cast(int64x2_t, __s0), __p1));   __ret; })
#define vgetq_lane_u16(__p0, __p1) __extension__ ({   uint16_t __ret;   uint16x8_t __s0 = __p0;   __ret = __builtin_bit_cast(uint16_t, __builtin_neon_vgetq_lane_i16(__builtin_bit_cast(int16x8_t, __s0), __p1));   __ret; })
#define vget_lane_u64(__p0, __p1) __extension__ ({   uint64_t __ret;   uint64x1_t __s0 = __p0;   __ret = __builtin_bit_cast(uint64_t, __builtin_neon_vget_lane_i64(__builtin_bit_cast(int64x1_t, __s0), __p1));   __ret; })
#define vld1q_u8(__p0) __extension__ ({   uint8x16_t __ret;   __ret = __builtin_bit_cast(uint8x16_t, __builtin_neon_vld1q_v(__p0, 48));   __ret; })
#define vld1q_s8(__p0) __extension__ ({   int8x16_t __ret;   __ret = __builtin_bit_cast(int8x16_t, __builtin_neon_vld1q_v(__p0, 32));   __ret; })
#define vshlq_n_u8(__p0, __p1) __extension__ ({   uint8x16_t __ret;   uint8x16_t __s0 = __p0;   __ret = __builtin_bit_cast(uint8x16_t, __builtin_neon_vshlq_n_v(__builtin_bit_cast(int8x16_t, __s0), __p1, 48));   __ret; })
#define vshrq_n_u8(__p0, __p1) __extension__ ({   uint8x16_t __ret;   uint8x16_t __s0 = __p0;   __ret = __builtin_bit_cast(uint8x16_t, __builtin_neon_vshrq_n_v(__builtin_bit_cast(int8x16_t, __s0), __p1, 48));   __ret; })
#define vshrn_n_u16(__p0, __p1) __extension__ ({   uint8x8_t __ret;   uint16x8_t __s0 = __p0;   __ret = __builtin_bit_cast(uint8x8_t, __builtin_neon_vshrn_n_v(__builtin_bit_cast(int8x16_t, __s0), __p1, 16));   __ret; })
#define vst1q_u8(__p0, __p1) __extension__ ({   uint8x16_t __s1 = __p1;   __builtin_neon_vst1q_v(__p0, __builtin_bit_cast(int8x16_t, __s1), 48); })
#define vst1q_s8(__p0, __p1) __extension__ ({   int8x16_t __s1 = __p1;   __builtin_neon_vst1q_v(__p0, __builtin_bit_cast(int8x16_t, __s1), 32); })
#define vst1_u8(__p0, __p1) __extension__ ({   uint8x8_t __s1 = __p1;   __builtin_neon_vst1_v(__p0, __builtin_bit_cast(int8x8_t, __s1), 16); })
#define INT_MAX   __INT_MAX__
#define LONG_MAX  __LONG_MAX__
#define INT_MIN   (-__INT_MAX__  -1)
#define UINT_MAX  (__INT_MAX__  *2U +1U)
#define CHAR_BIT  __CHAR_BIT__
#define ULLONG_MAX (__LONG_LONG_MAX__*2ULL+1ULL)
