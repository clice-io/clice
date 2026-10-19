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
