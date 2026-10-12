#include <cio.h>
#include <cwin.h>
#include "../third/widen/widen.h"
int direct() { fake_word word = fake_stdout; return widen_twice(word) + FAKE_EOF; }
