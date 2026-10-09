// - verify: server

int §(use)use(int x) {
    return x;
}

template <typename T>
int caller(T v) {
    return use(v);
}

int direct() {
    return use(1);
}

template <typename... Args>
struct Fmt {
    const char* §(str)str;
};

template <typename... Args>
const char* print(Fmt<Args...> fmt) {
    return fmt.str;
}

const char* run() {
    return print(Fmt<int>{"x"}) + Fmt<>{"y"}.str[0];
}

namespace lib {
int §(helper)helper();
}

using lib::helper;
