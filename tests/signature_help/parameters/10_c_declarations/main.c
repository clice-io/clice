#define bool _Bool

int area();
int area(int, int);
int toggle(bool on);

int total(void) {
    toggle(§(standard_bool)1);
    return area(§(redeclared)1, 2);
}
