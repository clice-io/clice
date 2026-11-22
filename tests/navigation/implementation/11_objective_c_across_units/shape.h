__attribute__((objc_root_class))
@interface Object
@end

@protocol Sized
@property(nonatomic) int size;
- (int)§(protocol)size;
@end

@protocol Measured
- (int)§(redeclared)area;
@end

@interface Shape : Object <Sized, Measured>
@property(nonatomic) int area;
@end

@interface Pen : Object
@end
