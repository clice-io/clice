/// # Required qualifier
///
/// - status: supported
/// - diagnostics: expected
///
/// An enumerator the bare name does not reach completes with the qualifier it
/// needs, matched against the bare name

// The completion prefixes dangle; the statements stay
// semicolon-terminated so later markers are not dragged into recovery.
enum class Color { Red, Green };

namespace ns {
enum Fruit { Apple, Banana };
}

void paint(Color c);

void bar(Color c) {
    Color d = Re§(initializer);
    paint(Gre§(argument));
    ns::Fruit f = App§(namespace_enum);
    switch(c) {
        case Re§(case_label): break;
    }
}
