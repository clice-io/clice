// Saved with Windows line endings by the test: a token spanning lines is cut
// at each line's end, the carriage return left out.
/* a block
   comment */
int read() {
    return 1;
}
const char* raw = R"(line1
line2)";
