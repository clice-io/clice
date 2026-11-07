#include "objclass.h"

extern struct objclass objects[];
extern struct objdescr obj_descr[];

int main(void) {
    return §(use)objects[1].cost + (§(use_descr)obj_descr[0].name != 0);
}
