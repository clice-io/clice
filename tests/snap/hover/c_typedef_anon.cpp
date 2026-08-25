/// # Type Information
///
/// ## Anonymous struct typedef — the classic C `typedef struct {…} Name`
///
/// - status: supported
/// - order: 12
/// - issues: clangd#2219
/// - flags: ["-x", "c", "-std=c11"]
///
/// Compiled as C11: clangd renders a misleading `struct Point` for the
/// alias of an anonymous struct; clice names the struct after its typedef,
/// so both the alias and a variable of it report a clean `Point` card.

// snap: the out-of-order designated initializer cannot compile as C++,
// so silently dropping the fixture's C flags fails the run.

/// A 2-D point.
typedef struct {
  int x, y;
} §(01_typedef)Point;

Point §(02_var)origin = {.y = 2, .x = 1};
