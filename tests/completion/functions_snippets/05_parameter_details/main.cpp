// A parameter with a default value drops out of the signature detail; a
// trailing `...` shows.
int configure(int timeout, int retries = 3);
int printf_like(const char* fmt, ...);

int x = confi§(default_argument);
int y = printf§(variadic);
