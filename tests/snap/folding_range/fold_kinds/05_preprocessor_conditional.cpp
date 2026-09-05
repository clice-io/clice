/// # Preprocessor conditional folding
///
/// - status: partial
/// - issues: clangd#1661, clangd#2059
/// - flags: ["-std=c++23"]
///
/// Conditional branches separated by `#else` form folding ranges
///
/// A bare `#if ... #endif` block without an `#else` does not fold yet.

#ifdef ENABLE_LOGGING    // ┐
void log_message();      // │ no fold yet: bare conditional without #else
#endif                   // ┘

#ifdef USE_THREADS       // ┐
void spawn_workers();    // │ folds: branches delimited by #else
#else                    // │
void run_inline();       // │
#endif                   // ┘

#ifdef USE_EPOLL         // ┐
void poll_epoll();       // │ no fold yet: the branch before #elifdef
#elifdef USE_KQUEUE      // │ ┐
void poll_kqueue();      // │ │ folds: the #elifdef branch, delimited by #else
#else                    // │ ┘
void poll_select();      // │
#endif                   // ┘
