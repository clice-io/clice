__attribute__((objc_root_class))
@interface Base
@property(nonatomic) int count;
@end

@interface Counter : Base
@property(nonatomic) int total;
@end

@interface Counter (Limits)
@property(nonatomic, readonly) int limit;
@end

@interface Leaf : Counter
@end

@implementation Base
- (int)§(base_count)count {
    return 0;
}
@end

@implementation Counter
- (int)§(total)total {
    return 0;
}

- (void)§(set_total)setTotal:(int)total {
}

- (int)§(count)count {
    return super.count;
}
@end

@implementation Leaf
- (int)§(leaf_count)count {
    return super.count;
}
@end

@implementation Counter (Limits)
- (int)§(limit)limit {
    return self.total;
}
@end

void §(bump)bump(Counter *counter) {
    counter.total += 1;
    [counter setTotal:[counter limit]];
    (void)counter.limit;
    (void)counter.count;
}
