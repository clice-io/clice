#import "shapes.h"

#define SIZE_SELECTOR @selector(size)

@implementation Box
@synthesize size = _size;

- (int)twice {
    return [self §(message)size] * 2;
}

- (SEL)action {
    return @selector(§(selector)size);
}

- (SEL)written_by_macro {
    return SIZE_SELECTOR;
}
@end
