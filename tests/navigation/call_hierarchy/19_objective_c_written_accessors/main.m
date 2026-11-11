__attribute__((objc_root_class))
@interface Base
@property(nonatomic) int count;
@property(class, nonatomic, readonly) int shared;
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

+ (int)shared {
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

+ (int)§(counter_shared)shared {
    return 1;
}
@end

@implementation Leaf
- (int)§(leaf_count)count {
    return super.count;
}

+ (int)§(leaf_shared)reset {
    return super.shared;
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

@interface Leaf (Sharing)
+ (int)fresh;
+ (int)again;
@end

@implementation Leaf (Sharing)
+ (int)§(fresh)fresh {
    return [super shared];
}

+ (int)§(again)again {
    return [self shared];
}
@end

__attribute__((objc_root_class))
@interface Meter
@property(nonatomic) int level;
@end

@interface Dial : Meter
@end

@interface Needle : Dial
@end

@implementation Meter
@end

@implementation Dial
- (int)§(dial_level)level {
    return 1;
}
@end

@implementation Needle
@end

int §(gauge)gauge(Needle *needle) {
    return needle.level;
}
