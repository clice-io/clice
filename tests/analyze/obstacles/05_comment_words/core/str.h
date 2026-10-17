#pragma once

// A comment's words are no code: `limit` is a const variable, its
// internal linkage no obstacle, whatever its comment says.
const int limit = 9;  // not static: const gives it internal linkage
const int other = 3;
