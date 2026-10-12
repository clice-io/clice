// Columns after non-ASCII text count UTF-16 code units: an accented letter is
// one, an emoji two.
const char* greeting = "héllo wörld 😀"; int after_text = 1;
/* café ☕ and 😀 */ int after_block = 2;
int after_comment = 3; // naïve 😀
