/// - verify: server

struct §(root)Widget {};

struct §(mid)Control: Widget {};

struct Button: Control {};

struct Label: Control {};

enum class §(leaf_enum)Mode : int {};

int §(gate_fn)helper() {
    return 0;
}
