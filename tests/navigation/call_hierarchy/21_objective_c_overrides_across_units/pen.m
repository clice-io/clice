#import "shape.h"

@implementation Pen
- (int)count {
    return 5;
}

- (void)setCount:(int)count {
}

- (int)twice {
    return [self count] * 2;
}
@end
