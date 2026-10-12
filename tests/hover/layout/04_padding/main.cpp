// Layout facts at the edges: tail padding, a union member (no offset), a
// bitfield in bits, a bitfield after another member, an empty class.

namespace studio::detail {
class Canvas {
    char la§(field)yer;
    double scale[2];
};
}

union Pixel {
    char ch§(union_field)annel;
    double opacity;
};

struct Permissions {
    unsigned int rea§(bitfield)d : 1;
};

struct PackedPermissions {
    char prefix;
    unsigned int wri§(bitfield_padding)te : 1;
    int suffix;
};

struct §(struct_size)EmptyCanvas {};
