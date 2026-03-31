export module Priv;
export int public_fn();
module :private;

int public_fn() {
    return 42;
}

int private_helper() {
    return 7;
}
