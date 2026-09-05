/// # Delegating constructors
///
/// - status: partial
/// - verify: server
///
/// Delegating-constructor parentheses navigate to the target constructor
///
/// A delegating constructor's target is reached from the opening parenthesis
/// of the delegated call. The constructor name itself resolves to the class
/// type, so navigation to the target constructor goes through the
/// parenthesis.

struct Widget {
    Widget(int w, int h);
    Widget() : §(delegate)Widget§(delegate_paren)(0, 0) {}
};
