#include "early.h"
int first();
#include "late.h"
int main() { return early() + late(); }
