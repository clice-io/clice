#import "shape.h"

#define SYNTHESIZE(name) @synthesize name = _##name;

@implementation Shape
SYNTHESIZE(size)

- (int)twice {
    return [self §(synthesized)size] * 2;
}
@end
