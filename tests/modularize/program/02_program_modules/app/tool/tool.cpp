#include "tool/tool.h"
#include "core/sink.h"
int tool::run(const core::Text& text, core::Sink* sink, core::Box<int>* box) {
    return text.size() + sink->lines + CORE_TWICE(1) + (box ? box->value : 0);
}
#if defined(SINK_H)
int tool_sink = 1;
#endif
