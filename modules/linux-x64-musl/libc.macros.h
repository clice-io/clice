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
#define FLT_EVAL_METHOD __FLT_EVAL_METHOD__
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
#define WCHAR_MAX (0x7fffffff+L'\0')
#define _mm256_extract_epi64(X, N)   ((long long)__builtin_ia32_vec_ext_v4di((__v4di)(__m256i)(X), (int)(N)))
