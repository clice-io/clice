/// # Completion inside macro arguments — member access written as a macro argument completes as it would outside the macro
///
/// - status: supported
/// - diagnostics: expected

#define WRAP(...) __VA_ARGS__

struct Config {
    int retries;
    int timeout;
};

void run() {
    Config config;
    WRAP(config.§(argument));
}
