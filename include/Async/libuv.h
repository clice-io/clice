#pragma once

#ifdef _WIN32
#define NOMINMAX
#endif

#include "uv.h"

#ifdef _WIN32
#undef THIS
#endif

#include <cassert>
#include <expected>
#include <type_traits>
#include <system_error>

#include "Support/TypeTraits.h"
#include "Support/Logging.h"

namespace clice::async {

/// The default event loop.
extern uv_loop_t* loop;

template <typename T, typename U>
T& uv_cast(U* u) {
    assert(u && u->data && "uv_cast: invalid uv handle");
    return *static_cast<std::remove_cvref_t<T>*>(u->data);
}

#define UV_TYPE_ITER(_, name) || std::is_same_v<T, uv_##name##_t>

/// Check if the type `T` is a libuv handle.
template <typename T>
constexpr bool is_uv_handle_v = false UV_HANDLE_TYPE_MAP(UV_TYPE_ITER);

/// Check if the type `T` is a libuv request.
template <typename T>
constexpr bool is_uv_req_v = false UV_REQ_TYPE_MAP(UV_TYPE_ITER);

template <typename T>
constexpr bool is_uv_stream_v = std::is_same_v<T, uv_stream_t> || std::is_same_v<T, uv_tcp_t> ||
                                std::is_same_v<T, uv_pipe_t> || std::is_same_v<T, uv_tty_t>;

#undef UV_TYPE_ITER

template <typename T, typename U>
T* uv_cast(U& u) {
    if constexpr(std::is_same_v<T, uv_handle_t>) {
        static_assert(is_uv_handle_v<std::remove_cvref_t<U>>, "uv_cast: invalid uv handle");
    } else if constexpr(std::is_same_v<T, uv_req_t>) {
        static_assert(is_uv_req_v<std::remove_cvref_t<U>>, "uv_cast: invalid uv request");
    } else if constexpr(std::is_same_v<T, uv_stream_t>) {
        static_assert(is_uv_stream_v<std::remove_cvref_t<U>>, "uv_cast: invalid uv stream");
    } else {
        static_assert(dependent_false<U>, "uv_cast: invalid type");
    }
    return reinterpret_cast<T*>(&u);
}

void uv_check_result(const int result,
                     const std::source_location location = std::source_location::current());

template <typename T>
class Task;

template <typename T>
using Result = Task<std::expected<T, std::error_code>>;

const std::error_category& category();

void init();

void run();

void stop();

}  // namespace clice::async
