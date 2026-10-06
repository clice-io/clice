module;

#if defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)
#include "linux-x64/spdlog.fragment.h"
#elif defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)
#include "linux-x64-debug/spdlog.fragment.h"
#elif defined(__linux__) && defined(__aarch64__)
#include "linux-arm64/spdlog.fragment.h"
#elif defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)
#include "macos-arm64/spdlog.fragment.h"
#elif defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)
#include "macos-arm64-debug/spdlog.fragment.h"
#elif defined(__APPLE__) && defined(__x86_64__)
#include "macos-x64/spdlog.fragment.h"
#else
#error "no configuration merged matches this compilation"
#endif

export module spdlog;

export namespace spdlog {
namespace fmt_lib = ::std;
using ::spdlog::apply_all;
using ::spdlog::apply_logger_env_levels;
using ::spdlog::color_mode;
using ::spdlog::create;
using ::spdlog::critical;
using ::spdlog::debug;
using ::spdlog::default_factory;
using ::spdlog::default_logger;
using ::spdlog::default_logger_raw;
using ::spdlog::disable_backtrace;
using ::spdlog::drop;
using ::spdlog::drop_all;
using ::spdlog::dump_backtrace;
using ::spdlog::enable_backtrace;
using ::spdlog::err_handler;
using ::spdlog::error;
using ::spdlog::file_event_handlers;
using ::spdlog::filename_t;
using ::spdlog::flush_every;
using ::spdlog::flush_on;
using ::spdlog::format_string_t;
using ::spdlog::formatter;
using ::spdlog::get;
using ::spdlog::get_level;
using ::spdlog::info;
using ::spdlog::initialize_logger;
using ::spdlog::is_convertible_to_any_format_string;
using ::spdlog::is_convertible_to_basic_format_string;
using ::spdlog::level_t;
using ::spdlog::log;
using ::spdlog::log_clock;
using ::spdlog::logger;
using ::spdlog::memory_buf_t;
using ::spdlog::pattern_time_type;
using ::spdlog::register_logger;
using ::spdlog::register_or_replace;
using ::spdlog::set_automatic_registration;
using ::spdlog::set_default_logger;
using ::spdlog::set_error_handler;
using ::spdlog::set_formatter;
using ::spdlog::set_level;
using ::spdlog::set_pattern;
using ::spdlog::should_log;
using ::spdlog::shutdown;
using ::spdlog::sink_ptr;
using ::spdlog::sinks_init_list;
using ::spdlog::source_loc;
using ::spdlog::spdlog_ex;
using ::spdlog::string_view_t;
using ::spdlog::swap;
using ::spdlog::synchronous_factory;
using ::spdlog::throw_spdlog_ex;
using ::spdlog::trace;
using ::spdlog::warn;
}

export namespace spdlog::details {
using ::spdlog::details::backtracer;
using ::spdlog::details::circular_q;
using ::spdlog::details::conditional_static_cast;
using ::spdlog::details::enable_if_t;
using ::spdlog::details::log_msg;
using ::spdlog::details::log_msg_buffer;
using ::spdlog::details::make_unique;
using ::spdlog::details::null_atomic_int;
using ::spdlog::details::null_mutex;
using ::spdlog::details::periodic_worker;
using ::spdlog::details::registry;
using ::spdlog::details::thread_pool;
using ::spdlog::details::to_string_view;
}

export namespace spdlog::level {
using ::spdlog::level::critical;
using ::spdlog::level::debug;
using ::spdlog::level::err;
using ::spdlog::level::from_str;
using ::spdlog::level::info;
using ::spdlog::level::level_enum;
using ::spdlog::level::n_levels;
using ::spdlog::level::off;
using ::spdlog::level::to_short_c_str;
using ::spdlog::level::to_string_view;
using ::spdlog::level::trace;
using ::spdlog::level::warn;
}

export namespace spdlog::sinks {
using ::spdlog::sinks::base_sink;
using ::spdlog::sinks::ringbuffer_sink;
using ::spdlog::sinks::ringbuffer_sink_mt;
using ::spdlog::sinks::ringbuffer_sink_st;
using ::spdlog::sinks::sink;
}
