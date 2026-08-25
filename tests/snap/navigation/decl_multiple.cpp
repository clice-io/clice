/// # Go to Declaration
///
/// ## Multiple declarations — every declaration site
///
/// - status: supported
/// - verify: server
/// - order: 6
///
/// When an entity is declared in several places, go-to-declaration on a
/// use lists every declaration site, not only the nearest one.

int §(first)clamp(int value);
int §(second)clamp(int value);

int clamp(int value) {
    return value < 0 ? 0 : value;
}

int hold(int value) {
    return §(use)clamp(value);
}
