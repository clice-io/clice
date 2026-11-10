@class Pen;
@protocol Inking;

@protocol §(inking)Inking
- (void)refill;
@end

@protocol §(writing)Writing <Inking>
- (void)write;
@end

__attribute__((objc_root_class))
@interface Object
+ (instancetype)alloc;
- (instancetype)init;
@end

@interface Box<ObjectType> : Object
- (ObjectType)first;
@end

@interface §(pen_class)Pen : Object {
    int width;
}
@property(nonatomic) int width;
@property(nonatomic) int ink;
- (void):(int)x tail:(int)y;
+ (void)draw;
- (void)draw;
@end

@interface Pen (Extras) <Writing>
- (void)write;
@end

@interface Object (Extras)
- (void)tidy;
@end

@compatibility_alias Quill Pen;

@implementation Pen : Object
@synthesize width;

- (void):(int)x tail:(int)y {
    width = x + y;
    _ink = width;
}

+ (void)draw {
}

- (void)draw {
}
@end

@implementation Pen (Extras)
- (void)write {
}

- (void)refill {
}
@end

void use(Quill *quill, Class kind, id thing, Box<Pen *> *box) {
    [quill :1 tail:2];
    [quill setInk:[quill ink] + 1];
    [kind draw];
    [thing draw];
    id writing = @protocol(Writing);
    [[box first] write];
    (void)writing;
}
