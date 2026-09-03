/// # Prepare type hierarchy on class, struct, enum and union
///
/// - status: supported
/// - verify: server
///
/// Preparing a type hierarchy anchors an item on any user-defined type
/// tag — class, struct, enum and union alike.

class §(cls)Handle {};

struct §(strct)Point {};

enum class §(enm)Mode {};

union §(uni)Storage {
    int i;
    float f;
};
