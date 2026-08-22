/// - verify: server

struct §(root)Widget {};

struct §(mid)Control : Widget {};

struct Button : Control {};

struct Label : Control {};

struct Extra {};

struct §(multi)Panel : Control, Extra {};

union §(union_def)Packet {
    int raw;
    unsigned bits;
};

enum class §(leaf_enum)Mode : int {};

int §(gate_fn)helper() {
    return 0;
}
