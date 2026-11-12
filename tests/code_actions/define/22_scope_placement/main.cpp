namespace detail { void §(same_line_namespace)helper(); }

namespace ns { struct S { void §(same_line_class)f(); }; }

struct H { void §(declarator)f(); int* p() { return new int; } } const hs[] = {
    {},
};

typedef struct T { void §(typedef_declarator)g(); } Alias;

#define PACKED __attribute__((packed))

struct Packed { void §(attribute)f(); char c; } __attribute__((packed));

struct Macro { void §(macro_attribute)g(); char c; } PACKED;

void outer() {
    void §(block_scope)inner();
    inner();
}

struct A {
    void m() {
        void §(member_block_scope)nested();
        nested();
    }
};

auto l = [] {
    void §(lambda_block_scope)local();
    local();
};

int after;
