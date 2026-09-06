namespace alias_selection {
template <bool Select, typename Left, typename Right> struct choose {
    using type = Left;
};
template <typename Left, typename Right> struct choose<false, Left, Right> {
    using type = Right;
};
template <bool Select, typename Left, typename Right>
using selected = typename choose<Select, Left, Right>::type;

using integ§(02_typedef_chain)er = selected<true, long, double>;
}

struct Incomplete;
int storage;
auto incomplete_pointer = (Incom§(01_forward_struct_value)plete*)&storage;

#define DUPLICATE_FOUR(x) x, x, x, x
#define DUPLICATE_256(x) DUPLICATE_FOUR(DUPLICATE_FOUR(DUPLICATE_FOUR(DUPLICATE_FOUR(x))))
int hu§(03_big_initializer)ge[] = {DUPLICATE_256(1)};

namespace jump_target {
int adjust(int value) {
    if (value < 0) {
        goto §(04_goto_label)finish;
    }
    value *= 2;
§(05_label_def)finish:
    return value;
}
}

namespace custom_allocation {
struct ArenaObject {
    static void* operator new(unsigned long count);
    static void operator delete(void* memory);
};
ArenaObject* object = §(06_operator_new)new ArenaObject;
void release() {
    §(07_operator_delete)delete object;
}
}

namespace qualified_allocation {
struct Record {
    static void* operator new(unsigned long count);
    Record(long);
};
Record* record = ::§(08_global_new)new Record(4);
Record direct§(09_ctor_paren)(5);
}

namespace overloaded_punctuation {
struct Table {
    int operator()(int row);
    int operator[](int row);
};
int read(Table table) {
    return table§(10_op_call)(3) + table§(11_op_subscript)[4];
}
}
