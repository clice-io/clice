/// @section Refinements
/// @title Inactive preprocessor branch indication — visually distinguish or auto-fold inactive `#if`/`#else` branches
/// @status partial
/// @order 3
///
/// The server already emits fold ranges for both branches of an
/// `#if`/`#else`, so clients can fold either branch manually. Knowing which
/// branch is _inactive_ — to dim or auto-fold it — is not implemented here;
/// that information belongs to the inactive-regions feature.
///
/// > **Note**: this overlaps with semantic tokens (inactive code dimming) and
/// > is partly a client UX concern. The server can mark these ranges with
/// > `FoldingRangeKind.Region` and clients can choose to auto-fold them.

#ifdef _WIN32
    // ... Windows code (active) ...
#else
    // ... POSIX code (inactive, could auto-fold) ...
#endif
