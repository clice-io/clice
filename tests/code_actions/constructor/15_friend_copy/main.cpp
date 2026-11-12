namespace std {
template <class T>
T&& move(T& value);
}

class Key {
    friend struct Door;
    Key(const Key&) = default;

public:
    Key() = default;
};

struct §(friend_class)Door {
    Key key;
    int turns;
};

class Token {
    template <class>
    friend struct Keeper;
    Token(const Token&) = default;

public:
    Token() = default;
};

template <class T>
struct §(friend_template)Keeper {
    Token token;
    T value;
};
