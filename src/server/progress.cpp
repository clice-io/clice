#include "server/progress.h"

#include "support/logging.h"

namespace clice {

namespace et = eventide;
namespace proto = eventide::language::protocol;

ProgressReporter::ProgressReporter(et::ipc::JsonPeer& peer, et::event_loop& loop)
    : peer_(peer), loop_(loop) {}

static proto::LSPObject make_begin_value(const std::string& title,
                                         const std::string& message,
                                         bool cancellable) {
    proto::LSPObject obj;
    obj["kind"] = proto::LSPAny(std::string("begin"));
    obj["title"] = proto::LSPAny(title);
    if(!message.empty())
        obj["message"] = proto::LSPAny(message);
    obj["cancellable"] = proto::LSPAny(cancellable);
    return obj;
}

static proto::LSPObject make_report_value(const std::string& message,
                                          std::optional<int> percentage) {
    proto::LSPObject obj;
    obj["kind"] = proto::LSPAny(std::string("report"));
    if(!message.empty())
        obj["message"] = proto::LSPAny(message);
    if(percentage.has_value())
        obj["percentage"] = proto::LSPAny(static_cast<proto::integer>(*percentage));
    return obj;
}

static proto::LSPObject make_end_value(const std::string& message) {
    proto::LSPObject obj;
    obj["kind"] = proto::LSPAny(std::string("end"));
    if(!message.empty())
        obj["message"] = proto::LSPAny(message);
    return obj;
}

et::task<bool> ProgressReporter::begin(const std::string& title,
                                       const std::string& message,
                                       bool cancellable) {
    if(active_)
        end();

    token_ = "clice-progress-" + std::to_string(next_token_++);

    proto::WorkDoneProgressCreateParams create_params;
    create_params.token = token_;

    auto result = co_await peer_.send_request(create_params);
    if(!result.has_value()) {
        LOG_WARN("ProgressReporter: client rejected progress token creation");
        co_return false;
    }

    active_ = true;

    proto::ProgressParams progress;
    progress.token = token_;
    progress.value = proto::LSPAny(make_begin_value(title, message, cancellable));
    peer_.send_notification(progress);

    co_return true;
}

void ProgressReporter::report(const std::string& message,
                              std::optional<int> percentage) {
    if(!active_)
        return;

    proto::ProgressParams progress;
    progress.token = token_;
    progress.value = proto::LSPAny(make_report_value(message, percentage));
    peer_.send_notification(progress);
}

void ProgressReporter::end(const std::string& message) {
    if(!active_)
        return;

    active_ = false;

    proto::ProgressParams progress;
    progress.token = token_;
    progress.value = proto::LSPAny(make_end_value(message));
    peer_.send_notification(progress);
}

}  // namespace clice
