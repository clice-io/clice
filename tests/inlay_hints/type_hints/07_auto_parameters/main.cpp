int twice(auto value) {
    return value + value;
}

int result = twice(21);

int measure(auto value) {
    return 1;
}

int a = measure(1);
int b = measure(2.0);

int spread(auto first, auto... rest, auto last) {
    return 0;
}

int c = spread<void*, char, float>(nullptr, 'x', 2.0f, 3);

template <typename T>
void body() {
    auto var = 42;
}

template void body<int>();
template void body<float>();
