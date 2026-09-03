/// # Static data member — to the in-class declaration
///
/// - status: supported
/// - verify: server
///
/// A static data member is declared inside the class and defined out of
/// line; go-to-declaration on a use offers the in-class declaration
/// alongside the definition.

struct Config {
    static int §(decl)timeout;
};

int Config::§(def)timeout = 30;

int read_config() {
    return Config::§(use)timeout;
}
