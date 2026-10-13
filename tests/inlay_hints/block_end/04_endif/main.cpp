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

#endif
