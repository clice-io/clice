// A parameter with a default value shows it; a trailing `...` shows.
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
};

}  // namespace std

std::vector<std::string> names;

int x = confi§(default_argument);
int y = printf§(variadic);

void use() {
    liste§(several_lines);
    retr§(macro_body);
    names.a§(standard_alias);
}
