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

namespace forward_declared {
struct Later;
§(before_definition)Later* later;
struct Later {
    int value;
};
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
