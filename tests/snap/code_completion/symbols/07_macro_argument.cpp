/// # Completion inside macro arguments
///
/// - status: supported
/// - diagnostics: expected
///
/// Member access written as a macro argument completes as it would outside the
/// macro

#define WRAP(...) __VA_ARGS__

struct Config {
    int retries;
    int timeout;
};

void run() {
    Config config;
    WRAP(config.§(argument));
}
