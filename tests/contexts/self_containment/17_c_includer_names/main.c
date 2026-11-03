#define WARN_UNUSED __attribute__((warn_unused_result))
#define DOC(name, text) static const char name[] = text

enum kind { K_A, K_B };
struct req { int x; };
struct entry { int code; };
struct encoding { int size; int kinds[4]; };

#include "proto.h"
#include "fields.h"
#include "entries.h"
#include "api.h"
#include "docs.h"

static const struct encoding ascii = {4,
    {
#include "kinds.h"
    }};

int main(void) { return ascii.size + entries[0].code; }
