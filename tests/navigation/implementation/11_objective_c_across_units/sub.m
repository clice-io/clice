#import "shape.h"

@interface Square : Shape
@end

@implementation Square
- (int)size {
    return 4;
}
@end

@interface Nib : Pen
@end

@implementation Nib
- (int)count {
    return 1;
}
@end

@interface Circle : Object <Sized>
@end

@implementation Circle
- (int)size {
    return 0;
}

- (void)setSize:(int)size {
}
@end
