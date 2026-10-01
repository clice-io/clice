/// # Preprocessor conditional folding
///
/// - status: supported
/// - issues: clangd#1661, clangd#2059
/// - flags: ["-std=c++23"]
///
/// Each branch of a conditional forms a folding range up to the directive that
/// ends it, which stays visible

#ifdef ENABLE_LOGGING    // ┐
void log_message();      // │ folds: a bare conditional
void log_flush();        // │
#endif                   // ┘

#ifdef USE_THREADS       // ┐
void spawn_workers();    // ┘ folds: the first branch
#else                    // ┐
void run_inline();       // ┘ folds: the #else branch
#endif

#ifdef USE_EPOLL         // ┐
void poll_epoll();       // ┘ folds: the branch before #elifdef
#elifdef USE_KQUEUE      // ┐
void poll_kqueue();      // ┘ folds: the #elifdef branch
#else                    // ┐
void poll_select();      // ┘ folds: the #else branch
#endif
