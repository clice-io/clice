__attribute__((objc_root_class))
@interface Object
@end

@protocol Sized
@property(nonatomic) int size;
- (int)size;
@end

@interface Box : Object <Sized>
@property(nonatomic) int count;
- (int)count;
@end

@implementation Box
@synthesize size = _size;

- (int)twice {
    return self.size + self.count;
}
@end
