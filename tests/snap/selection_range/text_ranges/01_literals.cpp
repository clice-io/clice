/// # String and character literals
///
/// - status: supported
///
/// The text of a string or character literal is selected without its quotes
/// before with them
///
/// An encoding prefix, a raw string's delimiter and a user-defined suffix
/// stay outside the text.

using size = decltype(sizeof(0));

struct Tag {};

Tag operator""_tag(const char*, size);

const char* plain = "plain §(plain)text";
const char8_t* prefixed = u8"prefixed §(prefixed)text";
const char* raw = R"delim(raw §(raw)text)delim";
Tag tagged = "tagged §(suffix)text"_tag;
char letter = '§(char)x';
