__attribute__((objc_root_class))
@interface Object
+ (instancetype)alloc;
- (instancetype)init;
@end

@interface Counter : Object
- (int)next;
@end

struct Tally {
    [[nodiscard]] int total() const {
        return count;
    }

    int count = 0;
};

int tally(Tally& tally) {
    Counter* counter = [[Counter alloc] init];
    auto step = [&] { return [counter next]; };
    return tally.total() + step();
}

[[using gnu: cold]] void fail();
[[deprecated("x")]] int legacy();
[[nodiscard, maybe_unused]] static int spare() {
    return 0;
}
[[gnu::cold]] void halt();
