/// # Constructors and destructors
///
/// - status: supported
/// - issues: clangd#1509, clangd#2078, clangd#872
///
/// Constructors and destructors use method tokens with dedicated modifiers
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
