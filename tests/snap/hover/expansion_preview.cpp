/// # Macro Hover
///
/// ## Fully-expanded preview — a function-like macro use shows its arguments substituted through the body
///
/// - status: supported
/// - order: 2
///
/// Hovering a function-like macro invocation shows the `#define` text and a
/// preview of the fully-expanded result with the call's arguments spliced in.

int x = 1, y = 2;

#define MAX(a, b) ((a) > (b) ? (a) : (b))

int z = §(01_expansion)MAX(x, y);
