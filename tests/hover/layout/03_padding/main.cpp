// Layout facts at the edges: tail padding, a union member (no offset), a
// bitfield in bits, a bitfield after another member, an empty class, a member
// of anonymous structs and unions (at its offset in the class around them),
// an array and a function pointer field hovered right after their names.

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

struct Message {
    int kind;
    union {
        struct {
            char flag;
            long cou§(anonymous_member)nt;
        };
        double ratio;
    };
};

struct Buffers {
    int slots§(array_field_end)[4];
    void (*notify§(pointer_field_end))(int);
};
