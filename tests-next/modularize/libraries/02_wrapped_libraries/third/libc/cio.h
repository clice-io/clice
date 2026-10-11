#pragma once
#define FAKE_EOF (-1)
extern int fake_stdout;
#define fake_stdout fake_stdout
#define CVA_TYPE_ONLY
#include <cva.h>
#undef CVA_TYPE_ONLY
