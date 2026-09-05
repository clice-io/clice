/// # Multiple declarations
///
/// - status: supported
/// - verify: server
///
/// Every declaration site
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
