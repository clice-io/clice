__attribute__((objc_root_class))
@interface Object
@end

@protocol §(protocol)Drawable
- (void)draw;
@end

@interface §(interface)Shape : Object <Drawable> {
    int _x;
}
- (void)draw;
@end

@implementation §(implementation)Shape
- (void)draw {
}
@end

@implementation §(implementation_only)Widget : Object
- (void)run {
}
@end

__attribute__((objc_subclassing_restricted, annotate("@final")))
@interface §(attributed)Final : Object
@end

@implementation Final
@end
