/// # Designator Hints
///
/// ## Broken initializers — designators survive next to initializers that fail to compile
///
/// - status: supported
/// - order: 6

// error-ok: the first initializer deliberately fails to convert.
struct Empty {};

struct Mixed {
    int a;
    int b;
};

void use() {
    Mixed m{Empty(), 1};
}
