#define BUILD_CHECK(condition, message)                                                        \
    do {                                                                                       \
        extern void build_check_failed(void) __attribute__((__error__(message)));             \
        if(!(condition))                                                                       \
            build_check_failed();                                                              \
    } while(0)

#define STORE_ONCE(x, value)                                                                   \
    do {                                                                                       \
        BUILD_CHECK(sizeof(x) == sizeof(char) || sizeof(x) == sizeof(short) ||                 \
                        sizeof(x) == sizeof(int) || sizeof(x) == sizeof(long),                 \
                    "unsupported access size");                                                \
        *(volatile __typeof__(x)*)&(x) = (value);                                              \
    } while(0)

struct node {
    int head;
};

void publish(struct node* node, int head) {
    §(statement)STORE_ONCE(node->head, head);
}
