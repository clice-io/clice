#import "shape.h"

@interface Shape ()
@property(nonatomic, copy) NSString *name;
@end

@implementation §(implementation)Shape
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
    §(synthesized_use)_x = x + y;
    §(ivar_use)_generation += 1;
    §(auto_ivar)_name = @"moved";
}

+ (instancetype)shapeWithX:(int)x {
    Shape *shape = [[self alloc] init];
    [shape §(message)setX:x §(second_piece)y:0];
    return shape;
}

- (void)draw {
}
@end

@implementation Shape (Extras)
- (void)wobble {
    [self setX:self.§(property_use)x + 1 y:0];
}
@end

@interface Circle : Shape
@end

@implementation Circle
- (void)draw {
    [super §(super_draw)draw];
}
@end

void render(id thing, id<§(protocol_use)Drawable> drawable, Shape *shape) {
    [thing §(untyped)draw];
    [drawable draw];
    [shape draw];
    Shape.§(class_property)count = 2;
    SEL selector = @selector(setX:y:);
    void (^block)(void) = ^{
      [shape §(category_method)wobble];
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
