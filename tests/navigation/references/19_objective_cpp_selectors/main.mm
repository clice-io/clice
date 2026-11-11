__attribute__((objc_root_class))
@interface Object
@end

@interface Canvas : Object
- (void)§(objc_draw)draw;
@end

@implementation Canvas
- (void)draw {
}
@end

struct Sketch {
    void §(cxx_draw)draw() {
    }
};

void paint(Canvas *canvas, id thing, Sketch &sketch, Sketch *copy) {
    [canvas draw];
    [thing draw];
    SEL action = @selector(draw);
    sketch.draw();
    copy->draw();
    void (Sketch::*member)() = &Sketch::draw;
    (void)action;
    (void)member;
}
