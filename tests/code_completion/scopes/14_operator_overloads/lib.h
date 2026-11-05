#pragma once

namespace lib {

struct Vec {};

bool operator==(Vec, int);
bool operator==(Vec, double);
bool operator==(Vec, Vec);
bool operator<(Vec, Vec);

void scale(Vec, int);
void scale(Vec, double);

}  // namespace lib
