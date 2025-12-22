#pragma once

#include <chrono>

#include "Awaiter.h"

namespace clice::async {

namespace awaiter {

struct sleep : uv<sleep, uv_timer_t, void> {
    std::chrono::milliseconds duration;

    int start(auto callback) {
        int err = uv_timer_init(async::loop, &request);
        if(err < 0) {
            return err;
        }
        return uv_timer_start(&request, callback, duration.count(), 0);
    }

    void cleanup() {
        error = uv_timer_stop(&request);
    }
};

}  // namespace awaiter

inline auto sleep(std::chrono::milliseconds duration) {
    return awaiter::sleep{{}, duration};
}

inline auto sleep(std::size_t milliseconds) {
    return sleep(std::chrono::milliseconds(milliseconds));
}

};  // namespace clice::async
