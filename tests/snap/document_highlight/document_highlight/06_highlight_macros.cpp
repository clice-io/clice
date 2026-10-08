/// # Macro names and arguments
///
/// - status: supported
///
/// A macro highlights at its definition, its expansions, the conditionals
/// testing it and its `#undef`
///
/// A name written in a macro argument highlights where it is written; a
/// name the macro's replacement spells highlights the whole invocation.
/// A macro used in another macro's replacement highlights neither there
/// nor at that macro's invocations.

#define §(macro_def)LIMIT 10
#define CLAMP(v) ((v) > LIMIT ? LIMIT : (v))
#define RESET_TOTAL total = 0

int §(total)total;

int clamp_total() {
#ifdef LIMIT
    total = CLAMP(§(arg)total);
#endif
    RESET_TOTAL;
    return §(macro_use)LIMIT;
}

#undef LIMIT
