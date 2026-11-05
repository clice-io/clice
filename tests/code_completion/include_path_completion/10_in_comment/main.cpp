// A directive inside a block comment is no directive: nothing completes. A
// comment inside a directive is none of its header name.
/*
#include <snap§(pos)
*/
#include /* the snapshot */ <snap§(after_comment)
