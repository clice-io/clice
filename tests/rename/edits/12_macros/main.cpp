#include "config.h"

char buffer[§(size)BUFFER_SIZE];
int doubled = APPLY(§(twice)TWICE, BUFFER_SIZE);
int clamped = CLAMP(§(limit)LIMIT + 1);
int pasted = CAT(BUFFER_, SIZE);

#if defined(LIMIT) && LIMIT > 100
int large = 1;
#endif

int main() { return buffer[0] + doubled + clamped + pasted; }
