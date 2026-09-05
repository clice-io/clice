/// # Broken initializers
///
/// - status: supported
/// - diagnostics: expected
///
/// Designators survive next to initializers that fail to compile

// The first initializer deliberately fails to convert.
struct Empty {};

struct Mixed {
    int a;
    int b;
};

void use() {
    Mixed m{Empty(), 1};
}
