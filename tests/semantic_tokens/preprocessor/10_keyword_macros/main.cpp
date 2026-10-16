#define private public
#define FORCE_INLINE inline

struct Exposed {
private:
    int hidden = 0;
};

FORCE_INLINE int twice(int value) {
    return value * 2;
}

struct Owner {
    compl Owner();
};
