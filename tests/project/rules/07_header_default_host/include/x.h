// The header lies outside the rule's patterns: it compiles in its host, which
// takes the rule's default command.
#ifndef FROM_HOST
#error missing FROM_HOST
#endif
inline int x() { return 0; }
