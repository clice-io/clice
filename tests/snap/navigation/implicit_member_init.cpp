/// # Implicit Code Navigation
///
/// ## Member initializer list — navigate to base and member constructors
///
/// - status: partial
/// - verify: server
/// - order: 9
///
/// The base and member constructors run by an initializer list are reached
/// from the opening parenthesis of each initializer. The initializer name
/// itself resolves to the base type or the member, so navigation to the
/// constructor goes through the parenthesis.

struct Base {
    Base(int x);
};

struct Logger {
    Logger(int level);
};

struct App : Base {
    Logger logger;
    App() : §(base_init)Base§(base_paren)(42), §(member_init)logger§(member_paren)(1) {}
};
