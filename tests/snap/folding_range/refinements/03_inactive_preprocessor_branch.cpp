/// # Inactive preprocessor branch indication
///
/// - status: partial
///
/// Inactive branches are not visually distinguished or folded automatically yet
///
/// Every branch, active or not, has a fold range and can be folded manually.
/// Knowing which branch is _inactive_ — to dim or auto-fold it — is not
/// implemented here; that information belongs to the inactive-regions feature.
///
/// > **Note**: this overlaps with semantic tokens (inactive code dimming) and
/// > is partly a client UX concern. The server can mark these ranges with
/// > `FoldingRangeKind.Region` and clients can choose to auto-fold them.

#ifdef _WIN32
    // ... Windows code (active) ...
#else
    // ... POSIX code (inactive, could auto-fold) ...
#endif
