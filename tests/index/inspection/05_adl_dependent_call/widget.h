#pragma once
#include "lib.h"
#include "point.h"
template <class T> int width(const lib::Pair<app::Point, T>& p) { return to_text(p.first).size(); }
