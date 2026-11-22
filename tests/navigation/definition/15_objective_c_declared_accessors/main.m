#import "shapes.h"

@implementation Box
@synthesize size = _size;

- (int)twice {
    return [self §(message)size] * 2;
}

- (SEL)action {
    return @selector(§(selector)size);
}
@end
