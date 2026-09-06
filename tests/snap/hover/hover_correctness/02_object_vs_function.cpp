/// # Most-vexing-parse
///
/// - status: supported
///
/// Direct initialization and a function declaration have distinct hover cards
///
/// The direct initialization appears as a variable, while the most-vexing
/// form appears as a function declaration.

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
