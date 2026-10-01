// - verify: both
// - flags: ["-fms-extensions", "--target=x86_64-unknown-linux-gnu"]
//
// The target is pinned: MinGW predefines `__declspec` as a macro.
//
// An MS `__declspec(property)` member lists like a field.

struct Holder {
    int get_value();
    void put_value(int);
    __declspec(property(get = get_value, put = put_value)) int value;
};
