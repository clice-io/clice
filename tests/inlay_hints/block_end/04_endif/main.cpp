#ifndef CONFIG_H
#define CONFIG_H

#ifdef ENABLE_LOGGING
int feature_1();
int feature_2();
int feature_3();
int feature_4();
int feature_5();
int feature_6();
int feature_7();
int feature_8();
#ifdef LOG_TO_FILE
int log_1();
int log_2();
int log_3();
int log_4();
int log_5();
int log_6();
int log_7();
int log_8();
#endif
#endif

#if defined(USE_THREADS) && THREAD_COUNT > 2
int feature_1();
int feature_2();
int feature_3();
int feature_4();
int feature_5();
int feature_6();
int feature_7();
int feature_8();
#else
int single_thread();
#endif

#ifdef ENABLE_TRACING
int feature_1();
int feature_2();
int feature_3();
int feature_4();
int feature_5();
int feature_6();
int feature_7();
int feature_8();
#endif  // ENABLE_TRACING

#if VERSION > 2
int short_conditional();
#endif

#if defined(ENABLE_COMPRESSION) && defined(ENABLE_ENCRYPTION) && PROTOCOL_VERSION >= 3
int feature_1();
int feature_2();
int feature_3();
int feature_4();
int feature_5();
int feature_6();
int feature_7();
int feature_8();
#endif

#endif
