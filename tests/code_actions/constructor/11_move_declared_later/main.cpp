// A declaration of std::move only after the class does not spare the include:
// the constructor's body looks the name up where the class stands.

struct Handle {
    Handle(Handle&&) = default;
};

struct §(before)Before {
    Handle handle;
};

namespace std {
template <class T>
T&& move(T& value);
}

struct §(after)After {
    Handle handle;
};
