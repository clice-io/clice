struct Config {
    int width;
    int height;
};

int process_data(const Config& cfg) {
    return cfg.width * cfg.height;
}
