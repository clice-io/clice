#import "shapes.h"

#define SIZE_SELECTOR @selector(size)

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

- (SEL)written_by_macro {
    return SIZE_SELECTOR;
}
@end
