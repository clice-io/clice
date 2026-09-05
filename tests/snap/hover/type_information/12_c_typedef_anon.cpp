/// # Anonymous struct typedef
///
/// - status: supported
/// - issues: clangd#2219
/// - flags: ["-x", "c", "-std=c11"]
///
/// C typedef hover names an anonymous struct after its alias
///
/// Both the alias and a variable of it report a clean `Point` card rather
/// than presenting the anonymous type as a separately named struct.

// snap: the out-of-order designated initializer cannot compile as C++,
// snap: so silently dropping the fixture's C flags fails the run.

/// A 2-D point.
typedef struct {
  int x, y;
} §(01_typedef)Point;

Point §(02_var)origin = {.y = 2, .x = 1};
