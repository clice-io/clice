int value = 1;

#define ID(x) x
#define CALL §(helper)helper()

void helper();

int copied = §(ID)ID(§(value)value);

void run() {
    §(CALL)CALL;
}
