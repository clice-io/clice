// A #define in the leading run of directives, which the server compiles
// into its preamble, has a card at the definition and at its uses.

#define §(definition)EARLY 1

int use = §(use)EARLY;
