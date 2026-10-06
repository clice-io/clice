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
#define	assert(e)	((void)0)
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
#define INT64_C(v)   (v ## LL)
#define UINT8_C(v)   (v)
#define UINT64_C(v)  (v ## ULL)
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
#define O_WRONLY        0x0001          /* open for writing only */
#define O_NONBLOCK      0x00000004      /* no delay */
#define O_APPEND        0x00000008      /* set append mode */
#define O_CREAT         0x00000200      /* create if nonexistant */
#define O_TRUNC         0x00000400      /* truncate to zero length */
#define AT_FDCWD        -2
#define F_GETFL         3               /* get file status flags */
#define F_SETFL         4               /* set file status flags */
#define SIGINT  2       /* interrupt */
#define SIGPIPE 13      /* write on a pipe with no one to read it */
#define SIGTERM 15      /* software termination signal from kill */
#define SIGSTOP 17      /* sendable stop signal not from tty */
#define SIGCONT 19      /* continue a stopped process */
#define SIGXFSZ 25      /* exceeded file size limit */
#define SIG_DFL         (void (*)(int))0
#define SIG_IGN         (void (*)(int))1
#define	_XOPEN_VERSION			600		/* [XSI] */
#define FLT_EVAL_METHOD __FLT_EVAL_METHOD__
#define FLT_RADIX __FLT_RADIX__
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define NULL __null
#define offsetof(t, d) __builtin_offsetof(t, d)
#define _mm256_extract_epi64(X, N)   ((long long)__builtin_ia32_vec_ext_v4di((__v4di)(__m256i)(X), (int)(N)))
#define INT_MAX   __INT_MAX__
#define LONG_MAX  __LONG_MAX__
#define INT_MIN   (-__INT_MAX__  -1)
#define UINT_MAX  (__INT_MAX__  *2U +1U)
#define CHAR_BIT  __CHAR_BIT__
#define ULLONG_MAX (__LONG_LONG_MAX__*2ULL+1ULL)
