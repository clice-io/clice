__attribute__((objc_root_class))
@interface Object
@end

@protocol Named
@property int count;
@end

@interface Base : Object
@property int width;
@end

@interface Shape : Base <Named>
@end

@interface Square : Shape
@end

@implementation Shape
@synthesize count = _count;
@synthesize width = _width;

- (int)total {
    [self §(setter)setCount:1];
    return [self §(protocol)count] + [self §(superclass)width];
}

- (SEL)action {
    return @selector(§(selector)count);
}
@end

int measure(Square *square) {
    return [square §(subclass)count];
}
