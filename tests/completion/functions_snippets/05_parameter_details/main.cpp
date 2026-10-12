// A parameter with a default value shows it; a trailing `...` shows.
int configure(int timeout, int retries = 3);
int printf_like(const char* fmt, ...);

int x = confi§(default_argument);
int y = printf§(variadic);
