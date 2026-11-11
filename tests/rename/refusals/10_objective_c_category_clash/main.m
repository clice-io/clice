__attribute__((objc_root_class))
@interface Shape
@end

@interface Shape (§(extras)Extras)
- (void)wobble;
@end

@interface Shape (Sizing)
- (int)size;
@end

@implementation Shape (Extras)
- (void)wobble {
}
@end

@implementation Shape (Sizing)
- (int)size {
    return 0;
}
@end
