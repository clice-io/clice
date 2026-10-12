template <typename T>
concept §(Small_def)Small = sizeof(T) <= 4;

template <§(Small_use)Small T>
void use_small(T value);

template <typename T>
    requires §(Small_use2)Small<T>
void require_small(T value);
