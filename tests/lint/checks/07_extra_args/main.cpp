// The configuration's ExtraArgs reach the compile: the code with the
// finding exists only under the define they add.
#ifdef RATIO_DIV
double ratio(int a, int b) { return a / b; }
#endif
int main() { return 0; }
