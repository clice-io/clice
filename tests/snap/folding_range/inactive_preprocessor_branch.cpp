/// # Refinements
///
/// ## Inactive preprocessor branch indication — visually distinguish or auto-fold inactive `#if`/`#else` branches
///
/// - status: partial
/// - order: 3
/// - snap: skip
///
/// The server emits a fold range for the region between the condition and
/// `#else`, so the first branch can be folded manually; the post-`#else`
/// branch gets no range yet. Knowing which branch is _inactive_ — to dim or
/// auto-fold it — is not implemented here; that information belongs to the
/// inactive-regions feature.
///
/// > **Note**: this overlaps with semantic tokens (inactive code dimming) and
/// > is partly a client UX concern. The server can mark these ranges with
/// > `FoldingRangeKind.Region` and clients can choose to auto-fold them.

// snap: skip because the leading directives fall into the server's
// preamble PCH and the server reply drops the conditionDirective fold that
// the inspect-path (no-PCH) compile reports. Un-skip once the two paths
// agree.

#ifdef _WIN32
    // ... Windows code (active) ...
#else
    // ... POSIX code (inactive, could auto-fold) ...
#endif
