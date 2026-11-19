__attribute__((objc_root_class))
@interface Base
@property int value;
@end

@interface Thing : Base
@end

@implementation Thing
@synthesize value;
@end

int read(Thing *thing) {
    return [thing value];
}
