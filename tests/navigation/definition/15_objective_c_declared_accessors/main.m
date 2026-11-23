#import "shapes.h"

@implementation Box
@synthesize size = _size;
@synthesize count = _count;

- (int)twice {
    return [self §(message)size] * 2;
}

- (SEL)action {
    return @selector(§(selector)size);
}

- (SEL)count_action {
    return @selector(§(class_selector)count);
}
@end
