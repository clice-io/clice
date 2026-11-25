#pragma once
#pragma clang system_header
#define alloca __builtin_alloca
#define	assert(x) (void)0
#define __LONG_MAX 0x7fffffffffffffffL
#define EPERM            1
#define ESRCH            3
#define EINTR            4
#define EINVAL          22
#define EPIPE           32
#define FLT_EVAL_METHOD 0
#define SIGHUP    1
#define SIGINT    2
#define SIGPIPE   13
#define SIGTERM   15
#define SIGCONT   18
#define SIGSTOP   19
#define SIGXFSZ   25
#define SIZE_MAX        UINT64_MAX
#define bswap_64(x) __bswap_64(x)
#define errno (*__errno_location())
#define FLT_RADIX 2
#define CHAR_BIT 8
#define INT_MIN  (-1-0x7fffffff)
#define INT_MAX  0x7fffffff
#define UINT_MAX 0xffffffffU
#define LONG_MAX __LONG_MAX
#define LLONG_MAX  0x7fffffffffffffffLL
#define ULLONG_MAX (2ULL*LLONG_MAX+1)
#define LC_CTYPE    0
#define LC_NUMERIC  1
#define LC_TIME     2
#define LC_COLLATE  3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL      6
#define LC_CTYPE_MASK    (1<<LC_CTYPE)
#define LC_NUMERIC_MASK  (1<<LC_NUMERIC)
#define LC_TIME_MASK     (1<<LC_TIME)
#define LC_COLLATE_MASK  (1<<LC_COLLATE)
#define LC_MONETARY_MASK (1<<LC_MONETARY)
#define LC_MESSAGES_MASK (1<<LC_MESSAGES)
#define LC_ALL_MASK      0x7fffffff
#define PTHREAD_MUTEX_INITIALIZER {{{0}}}
#define PTHREAD_COND_INITIALIZER {{{0}}}
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_NULL ((pthread_t)0)
#define SIG_DFL  ((void (*)(int)) 0)
#define SIG_IGN  ((void (*)(int)) 1)
#define va_start(v,l)   __builtin_va_start(v,l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v,l)     __builtin_va_arg(v,l)
#define offsetof(type, member) __builtin_offsetof(type, member)
#define INT64_MIN  (-1-0x7fffffffffffffff)
#define INT32_MAX  (0x7fffffff)
#define INT64_MAX  (0x7fffffffffffffff)
#define UINT32_MAX (0xffffffffu)
#define UINT64_MAX (0xffffffffffffffffu)
#define UINT8_C(c)  c
#define INT64_C(c) c ## L
#define UINT64_C(c) c ## UL
#define stdout (stdout)
#define stderr (stderr)
#define NULL nullptr
#define WCHAR_MAX (0xffffffffu+L'\0')
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
