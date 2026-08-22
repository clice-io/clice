/// - verify: server
///
/// A macro defined in a header generates symbols at invocations in the
/// source: argument-spelled names anchor at their spelling and
/// body-spelled names at the invocation, in both cases inside this file —
/// the definition living elsewhere no longer discards the rows.

#include "hooks.h"

§(macro_use)EXPORT_HOOK(§(hook_decl)advance);

EXPORT_HOOK(§(hook_def)advance) {
    return state + 1;
}

§(body_gen_site)COUNT_USES;

bool check() {
    return §(body_gen_use)uses_counted && §(hook_use)advance(1) > 0;
}
