#import "shape.h"

@interface Shape ()
@property(nonatomic, copy) NSString *name;
@end

@implementation Shape
@synthesize x = _x;

static int shapes;

+ (int)count {
    return shapes;
}

+ (void)setCount:(int)count {
    shapes = count;
}

- (int)area {
    return _x * _x;
}

- (void)setX:(int)x y:(int)y {
    _x = x + y;
    _generation += 1;
    _name = @"moved";
}

+ (instancetype)shapeWithX:(int)x {
    Shape *shape = [[self alloc] init];
    [shape setX:x y:0];
    return shape;
}

- (void)draw {
}
@end

@implementation Shape (Extras)
- (void)wobble {
    [self setX:self.x + 1 y:0];
}
@end

@interface Circle : Shape
@end

@implementation Circle
- (void)draw {
    [super draw];
}
@end

void render(id thing, id<Drawable> drawable, Shape *shape) {
    [thing draw];
    [drawable draw];
    [shape draw];
    Shape.count = 2;
    SEL selector = @selector(setX:y:);
    void (^block)(void) = ^{
      [shape wobble];
    };
    block();
    @try {
        [Shape shapeWithX:1];
    } @catch (Shape *error) {
        [error draw];
    } @finally {
    }
    (void)selector;
}

@interface Square : Shape
@end

@implementation Square
- (void)drawShape:(Shape *)shape {
    @try {
        [self draw];
    } @catch (...) {
        [shape draw];
    }
}
@end
