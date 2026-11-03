template <class T>
struct Text {
    void resize(int length);
    constexpr void reserve(int capacity);
};

template <class T>
void Text<T>::resize(int count) {}

template <class T>
constexpr void Text<T>::reserve(int requested) {}

inline void grow(Text<char>& text) {
    text.reserve(4);
}
