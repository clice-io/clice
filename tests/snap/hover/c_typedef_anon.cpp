/// # Type Information
///
/// ## Anonymous struct typedef — the classic `typedef struct {…} Name`
///
/// - status: supported
/// - order: 12
///
/// clangd tracks unhelpful hover on anonymous-struct typedefs as
/// clangd#2219; clice names the struct after its typedef, so both the alias
/// and a variable of it report a clean `Point` card.

namespace c_typedef_anon {

typedef struct {
  int x, y;
} §(01_typedef)Point;

Point §(02_var)origin;

}
