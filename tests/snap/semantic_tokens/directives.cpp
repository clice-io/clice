/// # Lexical Tokens
///
/// ## Preprocessor directives — `#if` chains keep directive kinds; disabled branches keep lexical kinds; pragma arguments stay plain
///
/// - status: supported
/// - order: 4

int before_conditional = 0;

#if 0
int disabled_branch;
#else
int enabled_branch = 1;
#endif

#define FLAG
#ifdef FLAG
int flagged = 2;
#endif

#pragma pack(1)

#
#define STRINGIZE(x) #x
const char* stringized = STRINGIZE(abc);
