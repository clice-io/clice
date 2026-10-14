// A namespace an included file closes folds from its brace to the end of
// this file, which holds no closing brace.
namespace outer {

int first();
int second();

#include "close.inc"
