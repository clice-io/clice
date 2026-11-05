#pragma once

namespace source {
int pick_source();
}  // namespace source

namespace api {

extern "C" int pick_c(int x);

namespace {
using source::pick_source;
}  // namespace

int pick_own();

}  // namespace api
