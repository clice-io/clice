#include "core/text.h"
#include "core/all.h"
#include "core/detail.h"
int core::Text::size() const { return detail() + CORE_TWICE(thing.v); }
