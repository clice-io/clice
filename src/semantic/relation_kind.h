#pragma once

#include <cstdint>

namespace clice {

struct RelationKind {
    enum Kind : std::uint32_t {
        Invalid = 0,
        Declaration,
        Definition,
        Reference,
        WeakReference,
        Read,
        Write,
        Interface,
        Implementation,
        TypeDefinition,
        Base,
        Derived,
        Constructor,
        Destructor,
        Caller,
        Callee,
    };

    constexpr RelationKind() = default;

    constexpr RelationKind(Kind kind) : kind_value(kind) {}

    constexpr operator Kind() const {
        return kind_value;
    }

    constexpr std::uint32_t value() const {
        return static_cast<std::uint32_t>(kind_value);
    }

    template <typename... Kinds>
    constexpr bool is_one_of(Kinds... kinds) const {
        return ((kind_value == kinds) || ...);
    }

    constexpr bool isDeclOrDef() const {
        return is_one_of(Declaration, Definition);
    }

    constexpr bool isReference() const {
        return is_one_of(Reference, WeakReference);
    }

    constexpr bool isBetweenSymbol() const {
        return is_one_of(Interface,
                         Implementation,
                         TypeDefinition,
                         Base,
                         Derived,
                         Constructor,
                         Destructor);
    }

    constexpr bool isCall() const {
        return is_one_of(Caller, Callee);
    }

private:
    Kind kind_value = Invalid;
};

constexpr bool operator==(RelationKind lhs, RelationKind rhs) {
    return lhs.value() == rhs.value();
}

constexpr auto operator<=>(RelationKind lhs, RelationKind rhs) {
    return lhs.value() <=> rhs.value();
}

constexpr bool operator&(RelationKind lhs, RelationKind rhs) {
    return lhs.value() == rhs.value();
}

}  // namespace clice
