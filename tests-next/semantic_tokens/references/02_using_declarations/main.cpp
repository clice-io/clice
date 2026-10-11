namespace tools {
inline int helper() {
    return 1;
}
struct Gadget {};
}

using tools::§(helper_use)helper;
using tools::§(Gadget_use)Gadget;

int used = §(helper_use2)helper();
§(Gadget_use2)Gadget gadget;
