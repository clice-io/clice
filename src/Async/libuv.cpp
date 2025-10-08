#include "Async/libuv.h"

namespace clice::async {

struct uv_error_category : public std::error_category {
    const char* name() const noexcept override {
        return "libuv";
    }

    std::string message(int code) const override {
#define UV_ERROR_HANDLE(name, message)                                                             \
    case UV_##name: return message;

        switch(code) {
            UV_ERRNO_MAP(UV_ERROR_HANDLE)
            default: return "unknown error";
        }

#undef UV_ERROR_HANDLE
    }
};

const std::error_category& category() {
    static uv_error_category instance;
    return instance;
}

/// Check the result of a libuv function call and log an error if it failed.
/// Use source_location to log the file, line, and function name where the error occurred.
void uv_check_result(const int result, const std::source_location location) {
    if(result < 0) {
        logging::warn("libuv error: {}", uv_strerror(result));
        logging::warn("At {}:{}:{}",
                      location.file_name(),
                      location.line(),
                      location.function_name());
    }
}

}  // namespace clice::async
