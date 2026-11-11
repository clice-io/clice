@class Pen;
@protocol Inking;

@protocol Inking
- (void)refill;
@end

@protocol Writing <Inking>
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

@interface Pen : Object {
    int §(width)width;
}
@property(nonatomic) int width;
@property(nonatomic) int ink;
@property(nonatomic, getter=isDry) int dry;
- (void):(int)x tail:(int)y;
- (int)size;
+ (void)draw;
- (void)draw;
@property(class, nonatomic, readonly) int count;
@end

@interface Pen (Extras) <Writing>
- (void)write;
@end

@interface Object (Extras)
- (void)tidy;
- (int)ink;
- (int)isDry;
- (void):(int)x tail:(int)y;
@end

@compatibility_alias Quill Pen;
@compatibility_alias Stylus Quill;

@interface Quill (Trim)
- (void)trim;
@end

@implementation Quill : Object
@synthesize width;

- (void):(int)x tail:(int)y {
    width = x + y;
    _ink = width;
}

- (int)size {
    return width;
}

+ (void)draw {
}

- (void)draw {
}

+ (int)count {
    return 0;
}
@end

@implementation Pen (Extras)
- (void)write {
    SEL action = @selector(draw);
    (void)action;
}

- (void)refill {
}
@end

void use(Quill *quill, Class kind, id thing, Box<Pen *> *box) {
    [quill :1 tail:2];
    [quill setInk:[quill ink] + 1];
    int size = quill.size;
    [kind draw];
    [thing draw];
    [thing ink];
    [thing isDry];
    [thing :3 tail:4];
    id writing = @protocol(Writing);
    [[box first] write];
    int count = Quill.count;
    (void)writing;
    (void)size;
    (void)count;
}
