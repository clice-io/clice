/// # Lexical Tokens
///
/// ## Inactive regions — tokens in untaken branches keep their lexical kinds and carry the `inactive` modifier; unclassified tokens become plain `identifier` carriers, so even a lone `}` line dims
///
/// - status: supported
/// - order: 5

int before = 0;

#if 0
int simple = 1;
bare identifiers;
call(arg);
"string in dead code";
// comment inside
#ifdef NESTED
int deeper = 2;
#endif
int tail = 3;
#endif

#if defined(MISSING)
first_branch;
#elif 0
elif_branch;
#else
int taken = 4;
#endif

#if 0
void edge() {
    inner(5);
}
#endif
