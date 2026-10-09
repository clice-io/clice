/// # Inactive preprocessor branches
///
/// - status: supported
///
/// Untaken branches fold like taken ones
///
/// Every branch of a conditional folds whether or not the compile takes it,
/// conditionals nested in an untaken branch included, so dead code can be
/// folded away by hand. Untaken code is dimmed by the `inactive` modifier of
/// semantic tokens; the folds themselves do not tell the branches apart.

#ifdef _WIN32
    // ... Windows code (untaken here) ...
#else
    // ... POSIX code (taken here) ...
#endif

#if 0
#ifdef NESTED
int nested_in_untaken;
#endif
int untaken;
#endif
