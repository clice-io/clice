/// # Literals
///
/// - status: supported
///
/// Numbers, characters and strings, including raw strings

int decimal = 42;
int hexadecimal = 0xFF;
double floating = 3.14;
char letter = 'x';
const char* text = "hello";
const char* raw = R"(no "escapes" in here)";
int after_raw = 1;

const char* multiline = R"(line1
line2
)"; int after_closing = 2;
