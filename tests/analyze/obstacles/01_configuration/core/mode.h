// main.cpp defines MODE_A, str.cpp does not: two readings declaring
// different entities.
#pragma once

#ifdef MODE_A
struct ModeA {};
#else
struct ModeB {};
#endif
