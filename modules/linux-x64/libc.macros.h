#pragma once
# define alloca(size)	__builtin_alloca (size)
#define	EPERM		 1	/* Operation not permitted */
#define	ESRCH		 3	/* No such process */
#define	EINTR		 4	/* Interrupted system call */
#define	EINVAL		22	/* Invalid argument */
#define	EPIPE		32	/* Broken pipe */
#define	ERANGE		34	/* Math result not representable */
# define __ASSERT_VOID_CAST static_cast<void>
# define assert(expr)		(__ASSERT_VOID_CAST (0))
# define __bswap_constant_64(x)      (__extension__ ((((x) & 0xff00000000000000ull) >> 56)		      		     | (((x) & 0x00ff000000000000ull) >> 40)		      		     | (((x) & 0x0000ff0000000000ull) >> 24)		      		     | (((x) & 0x000000ff00000000ull) >> 8)		      		     | (((x) & 0x00000000ff000000ull) << 8)		      		     | (((x) & 0x0000000000ff0000ull) << 24)		      		     | (((x) & 0x000000000000ff00ull) << 40)		      		     | (((x) & 0x00000000000000ffull) << 56)))
#  define __bswap_64(x)      (__extension__							            ({ __uint64_t __v, __x = (x);					      	 if (__builtin_constant_p (__x))				      	   __v = __bswap_constant_64 (__x);				      	 else								      	   __asm__ ("bswap %q0" : "=r" (__v) : "0" (__x));		      	 __v; }))
#   define errno (*__errno_location ())
#define __LC_CTYPE		 0
#define __LC_NUMERIC		 1
#define __LC_TIME		 2
#define __LC_COLLATE		 3
#define __LC_MONETARY		 4
#define __LC_MESSAGES		 5
#define __LC_ALL		 6
#define __LC_PAPER		 7
#define __LC_NAME		 8
#define __LC_ADDRESS		 9
#define __LC_TELEPHONE		10
#define __LC_MEASUREMENT	11
#define __LC_IDENTIFICATION	12
# define __PTHREAD_SPINS             0, 0
#define SIG_DFL	((__sighandler_t) 0)		/* Default action.  */
#define SIG_IGN	((__sighandler_t) 1)		/* Ignore signal.  */
#define	SIGINT		2	/* Interrupt (ANSI).  */
#define	SIGPIPE		13	/* Broken pipe (POSIX).  */
#define	SIGTERM		15	/* Termination (ANSI).  */
#define	SIGCONT		18	/* Continue (POSIX).  */
#define	SIGSTOP		19	/* Stop, unblockable (POSIX).  */
#define	SIGXFSZ		25	/* File size limit exceeded (4.2 BSD).  */
# define bswap_64(x) __bswap_64 (x)
# define SEEK_END	2	/* Seek from end of file.  */
#define	__GLIBC__	2
#define	__GLIBC_MINOR__	17
#define __GLIBC_PREREQ(maj, min) 	((__GLIBC__ << 16) + __GLIBC_MINOR__ >= ((maj) << 16) + (min))
#define	LC_ALL		  __LC_ALL
# define LC_CTYPE_MASK		(1 << __LC_CTYPE)
# define LC_NUMERIC_MASK	(1 << __LC_NUMERIC)
# define LC_TIME_MASK		(1 << __LC_TIME)
# define LC_COLLATE_MASK	(1 << __LC_COLLATE)
# define LC_MONETARY_MASK	(1 << __LC_MONETARY)
# define LC_MESSAGES_MASK	(1 << __LC_MESSAGES)
# define LC_PAPER_MASK		(1 << __LC_PAPER)
# define LC_NAME_MASK		(1 << __LC_NAME)
# define LC_ADDRESS_MASK	(1 << __LC_ADDRESS)
# define LC_TELEPHONE_MASK	(1 << __LC_TELEPHONE)
# define LC_MEASUREMENT_MASK	(1 << __LC_MEASUREMENT)
# define LC_IDENTIFICATION_MASK	(1 << __LC_IDENTIFICATION)
# define LC_ALL_MASK		(LC_CTYPE_MASK 				 | LC_NUMERIC_MASK 				 | LC_TIME_MASK 				 | LC_COLLATE_MASK 				 | LC_MONETARY_MASK 				 | LC_MESSAGES_MASK 				 | LC_PAPER_MASK 				 | LC_NAME_MASK 				 | LC_ADDRESS_MASK 				 | LC_TELEPHONE_MASK 				 | LC_MEASUREMENT_MASK 				 | LC_IDENTIFICATION_MASK 				 )
# define PTHREAD_MUTEX_INITIALIZER   { { 0, 0, 0, 0, 0, __PTHREAD_SPINS, { 0, 0 } } }
#define PTHREAD_COND_INITIALIZER { { 0, 0, 0, 0, 0, (void *) 0, 0, 0 } }
#define PTHREAD_ONCE_INIT 0
#  define __INT64_C(c)	c ## L
#  define __UINT64_C(c)	c ## UL
# define INT32_MAX		(2147483647)
# define INT64_MAX		(__INT64_C(9223372036854775807))
# define UINT32_MAX		(4294967295U)
# define UINT64_MAX		(__UINT64_C(18446744073709551615))
#  define SIZE_MAX		(18446744073709551615UL)
#  define INT64_C(c)	c ## L
# define UINT8_C(c)	c
#  define UINT64_C(c)	c ## UL
#define SEEK_END	2	/* Seek from end of file.  */
#define stdout stdout
#define stderr stderr
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
