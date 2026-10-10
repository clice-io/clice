export module Wrapper;
export import Core;

export int wrap_fn() {
    return core_fn() + 10;
}
