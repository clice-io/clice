/// # Parameter placeholder snippets
///
/// - status: supported
/// - config: {"bundle_overloads": false, "enable_function_arguments_snippet": true}
/// - diagnostics: expected
///
/// Calls insert tab-stop placeholders per argument; a no-argument function
/// stays plain text

// The completion prefixes dangle as unfinished statements.
int foooo(int x, float y);
void nothing_to_fill();

struct Foo {
    int bazzzz(int a, int b);
};

void bar() {
    Foo f;
    fo§(free_function);
    no§(no_arguments);
    f.ba§(method);
}
