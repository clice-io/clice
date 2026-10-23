#include §(include)"foo.h"§(past)
#define ANSWER 42

int main() {
    §(call)foo();
    Box box{1};
    return box.§(get)get() + §(macro)ANSWER;
}
