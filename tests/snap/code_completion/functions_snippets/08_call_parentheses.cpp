/// # Call parentheses
///
/// - status: supported
/// - config: {"insert_paren_in_function_call": true}
/// - diagnostics: expected
///
/// A completed call gets its parentheses with the cursor between them, unless
/// arguments already follow the name or it is not being called

// The completion prefixes dangle as unfinished statements.
namespace ns {
int compute(int x);

template <typename T>
int convert(T value);
}  // namespace ns

using namespace ns;

void bar() {
    int a = compu§(bare);
    int b = compu§(followed)(1);
    int c = conve§(template_arguments)<int>(1);
}

using ns::compu§(using_declaration);
