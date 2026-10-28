// The global module fragment folds up to a module declaration without
// `export` just as well.

module;

#define A 1
#define B 2

module demo:impl;

int x;
