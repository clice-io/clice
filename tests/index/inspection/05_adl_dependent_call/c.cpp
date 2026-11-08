#include "lib.h"
#include "text.h"
template <class T> int depth(const lib::Pair<app::Point, T>& p) { return to_text(p.first).size(); }
