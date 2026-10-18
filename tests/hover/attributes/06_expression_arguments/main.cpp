struct __attribute__((capability("mutex"))) Lock {};

Lock* lockp(int rq);

#define releases(x) __attribute__((release_capability(x)))

void unlock(int rq) releases(lockp(rq));

void use() {
    §(call)unlock(1);
}
