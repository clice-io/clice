/// # Deduced return type
///
/// - status: supported
///
/// A function's deduced `auto` return type expands to the deduced type, spelled for the function's scope

namespace ns {
struct Config {};
§(inside)auto load() { return Config{}; }
}

§(outside)auto make() { return ns::load(); }
