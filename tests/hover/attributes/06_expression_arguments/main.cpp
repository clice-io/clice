struct __attribute__((capability("mutex"))) Lock {};

Lock* lockp(int rq);

#define releases(x) __attribute__((release_capability(x)))

void unlock(int rq) releases(lockp(rq));

#define UNLOCK_FN(name, arg) void name(int arg) __attribute__((release_capability(lockp(arg))))

UNLOCK_FN(unlock_all, rq);

void use() {
    §(call)unlock(1);
    §(declared_by_macro)unlock_all(1);
}
