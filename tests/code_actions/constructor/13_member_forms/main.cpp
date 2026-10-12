// Fields as everyday classes write them: a default member initializer is a
// parameter like any other field and a static member is none; bit-fields
// take their type; a class template's fields of a parameter type are taken
// by value and moved from. An array field or an anonymous union offers no
// constructor.

struct §(initialized)Settings {
    int retries = 3;
    int timeout;
    static int instances;
};

struct §(bit_fields)Flags {
    unsigned visible : 1;
    unsigned level : 7;
};

template <class T>
struct §(template_fields)Range {
    T first;
    T last;
};

struct §(array)Buffer {
    char bytes[16];
    int used;
};

struct §(anonymous_union)Value {
    int kind;
    union {
        int number;
        float real;
    };
};
