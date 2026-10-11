struct Config {
    static int §(decl)timeout;
};

int Config::§(def)timeout = 30;

int read_config() {
    return Config::§(use)timeout;
}
