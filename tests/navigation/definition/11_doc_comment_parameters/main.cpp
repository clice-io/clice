/// Scales a value.
/// @param §(documented)value the value to scale
/// \param §(backslash)factor how much
int scale(int value, int factor);

/// Holds a value.
/// @tparam §(template)T what it holds
template <typename T>
struct Box {
    T value;
};

int scale(int value, int factor) {
    return value * factor;
}

/// Adds two values.
/// @param [in] §(spaced)left,§(listed)right the values
int add(int left, int right);

/// Fits in a register.
/// @tparam §(constrained)T the type
template <typename T>
concept Small = sizeof(T) <= 8;

/// Holds a pointer.
/// @tparam §(partial)T what it points to
template <typename T>
struct Box<T*> {
    T* value;
};

/// @param §(unicode)数量 how many
int take(int 数量);
