/// # Call Hierarchy
///
/// ## Calls inside lambdas
///
/// - status: supported
/// - verify: server
/// - order: 8
///
/// A call written in a lambda body appears in the incoming calls of the
/// function it invokes, attributed to the function that encloses the
/// lambda.

void §(foo)foo() {}

void use() {
    auto task = [] {
        foo();
    };
    task();
}
