#import "shape.h"

#define SYNTHESIZE(name) @synthesize name = _##name;

@implementation Shape
SYNTHESIZE(size)
@synthesize area = _area;

- (int)twice {
    return [self §(synthesized)size] * 2;
}
@end

@implementation Tower
@synthesize depth = _depth;
@end
