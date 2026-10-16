// A macro defined under a condition that leaves code outside it guards
// nothing.

int §(unguarded)before();

#ifndef SETTING
#define SETTING 1
#endif
