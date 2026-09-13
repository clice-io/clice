// - verify: inspect

// Units with the same text and no dependencies share one content: each
// of the three trailing assertions sees the same line above it.
static_assert(true, "");
static_assert(true, "");
static_assert(true, "");
static_assert(true, "");
