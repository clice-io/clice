#pragma once
#define ENTRY(name) inline const char* name##_name = #name;
#include "table.inc"
#undef ENTRY
