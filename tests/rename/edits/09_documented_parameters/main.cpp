/// Scales a value.
/// @param value what to scale
/// @param factor how much
/// @tparam §(template)T the value's type
template <typename T>
T scale(T value, int §(parameter)factor) {
    return value * factor;
}

/// \param §(comment)amount what to add
int add(int amount);

int main() { return scale(1, 2) + add(3); }
