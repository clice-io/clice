#ifndef ALPHA_H
#define ALPHA_H

// As musl's <stdio.h> declares stdout.
extern int counter;
#define counter (counter)

#endif
