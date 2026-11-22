#import "shape.h"

@implementation Shape
@synthesize count = _count;

- (int)twice {
    return [self count] * 2;
}
@end
