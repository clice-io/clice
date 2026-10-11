// A direct initialization with parentheses declares a variable; the empty
// parentheses of the most vexing parse declare a function.

namespace mvp {

struct Timer {
    Timer();
    Timer(int);
};

int seconds = 5;

void demo() {
    Timer act§(object_init)ive(seconds);
    Timer emp§(vexing_decl)ty();
}

}
