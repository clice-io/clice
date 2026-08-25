/// # Module Navigation
///
/// ## `import module_name` navigates to the module interface unit
///
/// - status: supported
/// - verify: server
/// - order: 1
/// - issues: clangd#2310
///
/// Go-to-definition on the name in an `import` declaration opens the
/// module interface unit that exports it, and uses of an imported symbol
/// reach its definition in that unit.

import §(module_name)widget;

int build() {
    return §(imported_use)area(2, 3);
}
