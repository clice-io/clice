/// # Expression Context
///
/// ## Record variables — enclosing constant value leaks in
///
/// - status: partial
/// - issues: clangd#1622
/// - order: 7
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
