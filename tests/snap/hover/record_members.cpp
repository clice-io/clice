// The member summary of a class: which members it lists, access sections,
// nested and unnamed types, templates.

namespace access_sections {
struct Base {
    int base_field;
};

class Widget : public Base {
    int id;

public:
    using Size = unsigned long;
    template <typename T> using Pointer = T*;
    static constexpr int limit = 4;
    enum class Kind { Small, Large };
    struct Node {
        int hidden;
    };
    struct Forward;
    template <typename T> struct Nested {
        T hidden;
    };

    Widget();
    ~Widget();
    void reset();
    operator bool() const;
    friend struct Base;
    using Base::base_field;
    static_assert(limit > 0);

protected:
    void helper();

private:
    static int instances;
    mutable int cache = 0;
    unsigned flags : 3;
};

void use() {
    §(class_sections)Widget widget;
    Widget::§(nested_type)Node node;
}
}

namespace functions_only {
struct Service {
    Service();
    void start();
    void stop();
};
§(functions_only)Service service;
}

namespace unnamed_members {
struct Value {
    enum { inline_capacity = 16 };
    int kind;
    union {
        long integer;
        double decimal;
    };
    struct {
        int x, y;
    } position, *previous;
    struct Named {
        int z;
    } named;
    alignas(8) union {
        int bits;
        float real;
    } aligned[2];
    typedef struct {
        int first, second;
    } Pair;
    static struct {
        int count;
    } shared;
};
§(unnamed_members)Value current;
}

namespace templates {
template <typename T> struct Box {
    using value_type = T;
    T value;
    template <typename U> static constexpr bool holds = false;
    template <typename U> static constexpr bool holds<U*> = true;
    template <typename U> struct Rebind {};
    template <typename U> struct Rebind<U*> {};
    struct Inner {
        T hidden;
    };
    void clear();
};

template <typename T> struct Box<T*> {
    T* pointer;
};

template <> struct Box<bool> {
    unsigned char bits;
};

template <typename T> struct §(primary_declaration)Box;
template <typename T> void take(§(primary_use)Box<T> box);
§(implicit_instantiation)Box<int> integer;
§(partial_specialization)Box<int*> pointer;
§(explicit_specialization)Box<bool> flag;
}

namespace instantiated_unnamed {
template <typename T> struct Expected {
    union {
        T value;
        int error;
    };
    struct {
        T low, high;
    } range, *last;
    enum { capacity = sizeof(T) };
    struct {
        T* next;
    } *lazy;
    bool has_error : 1;
};
§(instantiated_unnamed)Expected<long> expected;
}

namespace declarator_specifiers {
struct Device {
    volatile struct {
        unsigned control;
        unsigned status;
    } registers;
    mutable struct {
        int hits;
    } cache;
    const struct Limits {
        int low, high;
    } limits{0, 1};
    void attach(struct Driver* driver);
    typedef struct Handle* HandlePtr;
    struct {
        int id;
    } queue[sizeof(struct Request*)], *head;
};
§(declarator_specifiers)Device device;
}

namespace forward_declared {
struct Later;
§(before_definition)Later* later;
struct Later {
    int value;
};
}

namespace redeclared_nested {
class Tree {
    struct Node;
    template <typename T> struct Visitor;
    Node* root;
    struct Node {
        int value;
    };
    template <typename T> struct Visitor {};
};
§(redeclared_nested)Tree tree;
}

namespace derived_members {
struct Shape {
    int sides;
};
struct Square final : Shape {
    int length;
};
§(derived)Square square;
}
