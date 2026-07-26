// Reserved-character and unicode include paths must survive the URI round
// trip: the server keeps '+' literal while clients percent-encode it, '#'
// must travel as %23 rather than becoming a fragment, and UTF-8 names
// percent-encode per byte. ('?' is skipped: illegal in Windows filenames.)
#include "café.h"
#include "hash#header.h"
#include "plus+header.h"

int use_reserved = plus_value + hash_value + unicode_value;
