#ifndef OBJECTS_PASS_2_
#include "objclass.h"
#define OBJECT(name, desc, weight, cost) { name, desc }
struct objdescr obj_descr[] =
#else
#define OBJECT(name, desc, weight, cost) { weight, cost }
struct objclass §(second)objects[] =
#endif
{
OBJECT("arrow", "pointy stick", 1, 2),
OBJECT("dagger", 0, 10, 4),
};

#ifndef OBJECTS_PASS_2_
#undef OBJECT
#define OBJECTS_PASS_2_
#include "objects.c"

int objects_init(void) {
    return 0;
}
#endif
