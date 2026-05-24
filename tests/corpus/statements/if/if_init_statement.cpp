// C++17 if with init-statement
namespace if_init {

struct Result {
    int value;
    bool ok;
};

Result compute(int x) {
    if(x > 0)
        return {x * 10, true};
    return {0, false};
}

int with_init(int x) {
    if(auto r = compute(x); r.ok) {
        return r.value;
    }
    return -1;
}

// init-statement with array
bool find_char(const char* str, char target) {
    if(int i = 0; str) {
        while(str[i]) {
            if(str[i] == target)
                return true;
            ++i;
        }
    }
    return false;
}

// init-statement scoping: variables not visible outside
int scoping_demo(int x) {
    if(int doubled = x * 2; doubled > 10) {
        return doubled;
    } else {
        return doubled + 1;  // doubled visible in else
    }
}

void test() {
    [[maybe_unused]] int r1 = with_init(5);
    [[maybe_unused]] bool r2 = find_char("hello", 'l');
    [[maybe_unused]] int r3 = scoping_demo(3);
}

}  // namespace if_init
