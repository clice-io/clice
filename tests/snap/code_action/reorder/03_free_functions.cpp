/// # Free functions by declaration order
///
/// - status: supported
///
/// From a free function's definition, the definitions of the functions declared alongside it are reordered as declared

int alpha();
int beta();
int gamma();

int gamma() {
    return 3;
}

int §(alpha)alpha() {
    return 1;
}

int beta() {
    return 2;
}
