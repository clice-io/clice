#pragma once
#include "core/text.h"
namespace core {
struct Sink;
template <typename T>
struct Box;
template <typename T,
          typename U>
struct Pair;
namespace inner {
struct Deep;
}
}
namespace core::inner {
struct Deep;
}
namespace tool {
int run(const core::Text& text, core::Sink* sink, core::Box<int>* box = nullptr);
}
namespace tool::detail {
#define TOOL_DETAIL 1
}
