using size = decltype(sizeof(0));

struct Tag {};

Tag operator""_tag(const char*, size);

const char* plain = "plain §(plain)text";
const char8_t* prefixed = u8"prefixed §(prefixed)text";
const char* raw = R"delim(raw §(raw)text)delim";
Tag tagged = "tagged §(suffix)text"_tag;
char letter = '§(char)x';
