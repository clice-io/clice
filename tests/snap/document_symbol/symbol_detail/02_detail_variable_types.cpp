/// # Variable and field types
///
/// - status: supported
///
/// Variable details show the declared type, while lambdas render as `(lambda)`

namespace detail {

int timeout = 30;
const char* logger_name = "core";

struct Config {
    unsigned retries;
    double backoff;
};

auto on_error = [](int code) {
    return code != 0;
};

}  // namespace detail
