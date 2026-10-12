#include §(include)"foo.h"§(past)
#define ANSWER 42

int main() {
    §(call)foo();
    return §(macro)ANSWER;
}
