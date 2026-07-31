/// # Token Correctness
///
/// ## Constructors and destructors — method tokens with the constructor/destructor modifier
///
/// - status: supported
/// - issues: clangd#1509, clangd#2078, clangd#872
/// - order: 1
///
/// A destructor name renders as two tokens: the `~` carries the method
/// kind and the declaration/definition modifiers, the class name after it
/// stays a reference to the class.

struct Session {
    §Session();
    §~§Session();
};

Session::§Session() {}

Session::§~Session() {}

void destroy(Session* session) {
    session->§~Session();
}
