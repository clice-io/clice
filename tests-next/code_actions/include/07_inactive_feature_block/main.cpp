// A leading `#ifndef`/`#define`/`#endif` block the command line leaves
// inactive is no include guard either: with no include to follow, the
// directive goes to the start of the file, not into the block.

#ifndef MAX_SIZE
#define MAX_SIZE 64
#endif

std::§(vector)vector<int> values;
