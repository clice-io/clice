// An operator token shows the operator function it calls: class-specific
// new and delete, a global ::new running a constructor, a constructor call
// in parentheses, a call and a subscript operator, a != rewritten to the
// defaulted <=>, and the == a class template's defaulted friend <=> declares,
// without the comment above the <=>. A builtin -> has no card, a cast keyword
// its expression's.

namespace std {
struct strong_ordering {
    int value;
    constexpr operator int() const { return value; }
    static const strong_ordering equal;
    static const strong_ordering greater;
    static const strong_ordering less;
};
constexpr strong_ordering strong_ordering::equal = {0};
constexpr strong_ordering strong_ordering::greater = {1};
constexpr strong_ordering strong_ordering::less = {-1};
}

namespace custom_allocation {
struct ArenaObject {
    static void* operator new(unsigned long count);
    static void operator delete(void* memory);
};
ArenaObject* object = §(operator_new)new ArenaObject;
void release() {
    §(operator_delete)delete object;
}
}

namespace qualified_allocation {
struct Record {
    static void* operator new(unsigned long count);
    Record(long);
};
Record* record = ::§(global_new)new Record(4);
Record direct§(ctor_paren)(5);
}

namespace overloaded_punctuation {
struct Table {
    int operator()(int row);
    int operator[](int row);
};
int read(Table table) {
    return table§(op_call)(3) + table§(op_subscript)[4];
}
}

struct Revision {
    int number;
    auto operator<=>(const Revision&) const = default;
};

bool changed = Revision{2} !§(spaceship_neq)= Revision{3};

template <typename T>
struct Version {
    // Compares the parts in order.
    friend auto operator<=>(Version, Version) = default;
};

static_assert(Version<void>() =§(friend_spaceship_eq)= Version<void>());

namespace builtin_tokens {
struct Node {
    int value;
    int read() const { return this§(builtin_arrow)->value; }
};

long widened = static§(cast_keyword)_cast<long>(3);
}
