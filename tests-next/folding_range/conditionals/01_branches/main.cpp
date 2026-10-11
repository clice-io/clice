#ifdef ENABLE_LOGGING
void log_message();
void log_flush();
#endif

#ifdef USE_THREADS
void spawn_workers();
#else
void run_inline();
#endif

#ifdef USE_EPOLL
void poll_epoll();
#elifdef USE_KQUEUE
void poll_kqueue();
#else
void poll_select();
#endif
