__attribute__((objc_root_class))
@interface Object
@end

@protocol Named
@property int count;
@end

@protocol Sized
@property int size;
@end

@protocol Tagged
@property int tag;
@end

@interface Base : Object
@property int width;
@end

@interface Shape : Base <Named>
@end

@interface Shape () <Sized>
@end

@interface Shape (Tags) <Tagged>
@end

@interface Square : Shape
@end

@implementation Shape
@synthesize count = _count;
@synthesize size = _size;
@synthesize tag = _tag;
@synthesize width = _width;

- (int)total {
    [self §(setter)setCount:1];
    return [self §(protocol)count] + [self §(extension_protocol)size] +
           [self §(category_protocol)tag] + [self §(superclass)width];
}
@end

int measure(Square *square) {
    return [square §(subclass)count];
}
