#pragma once
#define LC_ALL_MASK			(  LC_COLLATE_MASK 					 | LC_CTYPE_MASK 					 | LC_MESSAGES_MASK 					 | LC_MONETARY_MASK 					 | LC_NUMERIC_MASK 					 | LC_TIME_MASK )
#define LC_COLLATE_MASK			(1 << 0)
#define LC_CTYPE_MASK			(1 << 1)
#define LC_MESSAGES_MASK		(1 << 2)
#define LC_MONETARY_MASK		(1 << 3)
#define LC_NUMERIC_MASK			(1 << 4)
#define LC_TIME_MASK			(1 << 5)
#define	stdout	__stdoutp
#define	stderr	__stderrp
#define	alloca(size)	__alloca(size)
#define	__alloca(size)	__builtin_alloca(size)
#define __ASSERT_FILE_NAME __FILE_NAME__
#define	assert(e)     (__builtin_expect(!(e), 0) ? __assert_rtn(__func__, __ASSERT_FILE_NAME, __LINE__, #e) : (void)0)
#define OSSwapInt64(x)  __DARWIN_OSSwapInt64(x)
#define __DARWIN_OSSwapConstInt64(x)     ((__uint64_t)((((__uint64_t)(x) & 0xff00000000000000ULL) >> 56) | 	        (((__uint64_t)(x) & 0x00ff000000000000ULL) >> 40) | 	        (((__uint64_t)(x) & 0x0000ff0000000000ULL) >> 24) | 	        (((__uint64_t)(x) & 0x000000ff00000000ULL) >>  8) | 	        (((__uint64_t)(x) & 0x00000000ff000000ULL) <<  8) | 	        (((__uint64_t)(x) & 0x0000000000ff0000ULL) << 24) | 	        (((__uint64_t)(x) & 0x000000000000ff00ULL) << 40) | 	        (((__uint64_t)(x) & 0x00000000000000ffULL) << 56)))
#define __DARWIN_OSSwapInt64(x)     (__builtin_constant_p(x) ? __DARWIN_OSSwapConstInt64(x) : _OSSwapInt64(x))
#define	LC_ALL		0
#define PTHREAD_MUTEX_INITIALIZER {_PTHREAD_MUTEX_SIG_init, {0}}
#define PTHREAD_COND_INITIALIZER {_PTHREAD_COND_SIG_init, {0}}
#define PTHREAD_ONCE_INIT {_PTHREAD_ONCE_SIG_init, {0}}
#define _PTHREAD_MUTEX_SIG_init		0x32AAABA7
#define _PTHREAD_COND_SIG_init		0x3CB0B1BB
#define _PTHREAD_ONCE_SIG_init		0x30B1BCBA
#define UINT8_C(v)   (v)
#define INT32_MAX        2147483647
#define INT64_MAX        9223372036854775807LL
#define UINT32_MAX        4294967295U
#define UINT64_MAX        18446744073709551615ULL
#define UINTPTR_MAX       18446744073709551615UL
#define SIZE_MAX          UINTPTR_MAX
#define SEEK_END        2       /* set file offset to EOF plus offset */
#define __restrict
#define errno (*__error())
#define EPERM           1               /* Operation not permitted */
#define ESRCH           3               /* No such process */
#define EINTR           4               /* Interrupted system call */
#define EINVAL          22              /* Invalid argument */
#define EPIPE           32              /* Broken pipe */
#define ERANGE          34              /* Result too large */
#define SIGINT  2       /* interrupt */
#define SIGPIPE 13      /* write on a pipe with no one to read it */
#define SIGTERM 15      /* software termination signal from kill */
#define SIGSTOP 17      /* sendable stop signal not from tty */
#define SIGCONT 19      /* continue a stopped process */
#define SIGXFSZ 25      /* exceeded file size limit */
#define SIG_DFL         (void (*)(int))0
#define SIG_IGN         (void (*)(int))1
#define	_XOPEN_VERSION			600		/* [XSI] */
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_HARDENING_MODE_DEFAULT 2
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
