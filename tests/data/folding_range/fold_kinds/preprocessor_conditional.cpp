/// # Preprocessor conditional folding (`#if` / `#ifdef` / `#ifndef` ... `#endif`)
///
/// - status: partial
/// - issues: clangd#1661, clangd#2059
/// - order: 5
///
/// Branch regions delimited by `#else` fold today; a bare `#if ... #endif`
/// block without an `#else` does not fold yet. clangd#2059 is a duplicate
/// of clangd#1661.

#ifdef ENABLE_LOGGING    // ┐
void log_message();      // │ no fold yet: bare conditional without #else
#endif                   // ┘

#ifdef USE_THREADS       // ┐
void spawn_workers();    // │ folds: branches delimited by #else
#else                    // │
void run_inline();       // │
#endif                   // ┘
