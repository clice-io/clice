/// # Symbol Detail
///
/// ## Variable and field types — the declared type in the `detail` field; lambdas render as `(lambda)`
///
/// - status: supported
/// - order: 2

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
