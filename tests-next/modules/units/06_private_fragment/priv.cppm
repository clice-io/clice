// What follows `module : private;` belongs to the interface alone: its
// importers see the declaration, not the definition.
export module Priv;
export int public_fn();
module : private;

int private_helper() {
    return 7;
}

int public_fn() {
    return §(defined)private_helper();
}
