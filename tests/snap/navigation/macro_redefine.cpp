/// - verify: server
///
/// Each `#define` of the same name is its own symbol: uses resolve to the
/// definition live at that point, and `#undef` counts as a reference of
/// the definition it cancels.

#define §(first_def)PHASE 1

int before = §(first_use)PHASE;

#undef §(undef_site)PHASE

#define §(second_def)PHASE 2

int after = §(second_use)PHASE;
