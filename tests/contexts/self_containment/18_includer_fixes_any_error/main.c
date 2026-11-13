double log1p(double);

struct row { int id; const char *name; };

static const struct row rows[] = {
#include "rows.h"
};

#include "smooth.h"

int dispatch(int op) {
#include "targets.h"
    goto *targets[op];
first:
    return rows[0].id;
second:
    return (int)smooth(rows[1].id);
}
