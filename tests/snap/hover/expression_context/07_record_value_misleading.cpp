/// # Record variables
///
/// - status: partial
/// - issues: clangd#1622
///
/// Enclosing constant value leaks in
///
/// Hovering a record-typed argument of a constant-evaluable call currently
/// reports that call's value (`Value = 7`) on the variable — a value that
/// is not the record's own.

namespace record_value_misleading {

struct Tag {};

constexpr int rank(Tag) {
  return 7;
}

void demo() {
  Tag t;
  int r = rank(§(01_record_arg)t);
}

}
