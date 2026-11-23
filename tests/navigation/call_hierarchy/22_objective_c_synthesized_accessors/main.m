__attribute__((objc_root_class))
@interface Object
@end

@protocol Sized
@property(nonatomic) int size;
- (int)size;
@end

@interface Box : Object <Sized>
@end

@interface Circle : Object <Sized>
@end

@interface Root : Object
- (int)count;
@end

@interface Base : Root
@property(nonatomic) int count;
@end

@interface Derived : Base
@end

@implementation Box
@synthesize size = _size;

- (int)§(twice)twice {
    return [self §(message)size] * 2;
}
@end

@implementation Circle
- (int)§(circle)size {
    return 1;
}

- (void)setSize:(int)size {
}
@end

@implementation Derived
- (int)§(derived)count {
    return 3;
}
@end

int tally(Root *root) {
    return [root count];
}
