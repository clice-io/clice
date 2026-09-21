/// # Free function declaration
///
/// - status: supported
///
/// A declared free function is defined right after its declaration, in the same namespace
///
/// The name needs no qualifier inside the namespace; the return type is
/// spelled for that scope.

namespace ns {
struct Config {};
Config §(load)load(const char* path);
}

int §(global)compute(int x);
