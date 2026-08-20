/// # Parameter Hints
///
/// ## Names from definitions — unnamed declaration parameters take the definition's names; leading underscores strip
///
/// - status: supported
/// - order: 6

void resize(int, int);

void fill(int _value, int __count);

int scale(int good);

void use() {
    resize(800, 600);
    fill(1, 2);
    // When both name their parameter, the declaration wins.
    scale(7);
}

void resize(int width, int height) {}

int scale(int bad) {
    return bad;
}
