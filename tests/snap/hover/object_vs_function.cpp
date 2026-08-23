/// # Hover Correctness
///
/// ## Most-vexing-parse — object init and function declaration hover distinctly
///
/// - status: supported
/// - order: 2
///
/// clangd tracks this as clangd#2225; clice reads the direct-init as a
/// variable and the vexing form as a function declaration.

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
