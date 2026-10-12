namespace ns {
struct Config {};
§(inside)auto load() { return Config{}; }
}

§(outside)auto make() { return ns::load(); }
