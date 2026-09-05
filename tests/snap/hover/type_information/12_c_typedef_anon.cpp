/// # Anonymous struct typedef
///
/// - status: supported
/// - issues: clangd#2219
/// - flags: ["-x", "c", "-std=c11"]
///
/// The classic C `typedef struct {…} Name`
///
/// Compiled as C11: clangd renders a misleading `struct Point` for the
/// alias of an anonymous struct; clice names the struct after its typedef,
/// so both the alias and a variable of it report a clean `Point` card.

// snap: the out-of-order designated initializer cannot compile as C++,
// snap: so silently dropping the fixture's C flags fails the run.

/// A 2-D point.
typedef struct {
  int x, y;
} §(01_typedef)Point;

Point §(02_var)origin = {.y = 2, .x = 1};
