/// Scales a value.
/// @tparam T The arithmetic type.
/// @param value The value to scale.
/// @param factor How much to scale by.
template <typename T>
T §(template_first)scale(T value, T factor);

/// Copies bytes.
/// @param to Where they go.
/// @param __count How many.
void copy(char* to, int __count);

void copy(char* destination, int __count) {}

int main() {
    char buffer[4];
    §(renamed)copy(buffer, 4);
}
