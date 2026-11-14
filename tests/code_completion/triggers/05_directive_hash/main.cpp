// A `#` opening a line, indented or not, lists the directives; one anywhere
// else completes nothing.
#§(line_start)
    #§(indented)
#define STRINGIZE(x) #§(stringize)x
const char* hash = "#§(in_string)";
// #§(in_comment)
/*
#§(in_block_comment)
*/
