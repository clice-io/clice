// Parameters show as their declarations write them: a default as written,
// even over several lines or in a macro's body, a pack's `...name`, an array,
// standard aliases, and the template arguments a call must write; a trailing
// `...` shows.
int configure(int timeout, int retries = 3, int delay = 10);
int printf_like(const char* fmt, ...);

#define DECLARE_RETRY(name) void name(int attempts = 3)

void listen(int port = 8000 +
                       80);
DECLARE_RETRY(retry);

namespace std {

template <typename Char>
struct basic_string {};

typedef basic_string<char> string;

template <typename T>
struct vector {
    T& at(int __index);

    template <typename U>
    U as(const T& __value);
};

}  // namespace std

std::vector<std::string> names;

template <typename T>
constexpr int size_of() {
    return sizeof(T);
}

template <decltype(size_of<int>()) N>
void frob(int count);

template <typename... Ts>
void emit(Ts... values);
void total(int values[4]);

int x = confi§(default_argument);
int y = printf§(variadic);

void use() {
    liste§(several_lines);
    retr§(macro_body);
    names.a§(standard_alias);
    emi§(pack);
    tota§(array);
    fro§(template_expression);
}
