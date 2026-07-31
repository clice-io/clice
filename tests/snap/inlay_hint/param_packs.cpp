/// # Parameter Hints
///
/// ## Unexpanded packs — a written pack expansion breaks the 1:1 argument mapping and stops hinting
///
/// - status: supported
/// - order: 10

void plot(int x, int y, int z);

template <typename... Ts>
void relay(Ts... ts) {
    // `ts...` may instantiate to any number of arguments.
    plot(0, ts...);
}

void use() {
    // The outer call still resolves through pack forwarding: 1 and 2 land
    // in plot's y and z.
    relay(1, 2);
}
