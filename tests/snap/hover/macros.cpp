/// - flags: ["-DFROM_CLI=7"]

// The anchor keeps every directive below out of the preamble region, so
// both verify paths see the live preprocessor record.
int anchor = 0;

#define §(01_object_def)LIMIT 64

#define §(02_function_def)CLAMP(x) ((x) < LIMIT ? (x) : LIMIT)

#define WRAP(x) CLAMP(x)

int use_object = §(03_object_use)LIMIT;

int use_function = §(04_function_use)CLAMP(10);

int use_nested = §(05_nested_use)WRAP(3);

#ifdef §(06_ifdef_use)LIMIT
int guarded = 1;
#endif

#undef §(07_undef_site)LIMIT

#define §(08_empty_def)BARE

§(09_empty_use)BARE int used_bare = 1;

int cli = §(10_cli_use)FROM_CLI;

#define §(11_multiline_def)MULTI(x) \
    ((x) + 1)

int folded = §(12_multiline_use)MULTI(2);

#define §(13_spliced_def)SPLIT_\
NAME 7

int spliced_use = §(14_spliced_use)SPLIT_NAME;

#define ECHO(x) x
#define INNER_VAL 99

int nested_arg = ECHO(§(15_nested_arg)INNER_VAL);

int joined_name = 8;

int continued = §(16_continued_arg)ECHO(joined_\
name);

// Doubling pastes grow one identifier token past the 1 KiB preview
// limit, so the expansion preview must truncate mid-token.
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define A16 aaaaaaaaaaaaaaaa
#define A32 CAT(A16, A16)
#define A64 CAT(A32, A32)
#define A128 CAT(A64, A64)
#define A256 CAT(A128, A128)
#define A512 CAT(A256, A256)
#define A1024 CAT(A512, A512)
#define A2048 CAT(A1024, A1024)

int §(17_oversized_use)A2048 = 1;
