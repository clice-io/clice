#define COUNT_SELECTOR @selector(count)
#define COUNT_PIECES (count)
#define SYNTHESIZE_DEPTH @synthesize depth = _depth;

__attribute__((objc_root_class))
@interface Object
@end

@protocol Named
@property int count;
@property int depth;
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
SYNTHESIZE_DEPTH

- (int)total {
    [self §(setter)setCount:1];
    return [self §(protocol)count] + [self §(superclass)width] + [self §(macro_body)depth];
}

- (SEL)action {
    return @selector(§(selector)count);
}

- (SEL)written_by_macro {
    return COUNT_SELECTOR;
}

- (SEL)pieces_by_macro {
    return @selector COUNT_PIECES;
}
@end

int measure(Square *square) {
    return [square §(subclass)count];
}
