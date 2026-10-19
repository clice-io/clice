struct §(record)tab_lp {
    int §(member)size;
};

#define GBR_LP struct §(in_body)tab_lp
#include "templ.inc"
#undef GBR_LP

#define FIELD size
static
#include "getter.inc"
