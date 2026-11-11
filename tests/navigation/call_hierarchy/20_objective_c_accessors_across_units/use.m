#import "counter.h"

@interface Tally : Counter
@end

@implementation Tally
- (int)§(tally_count)count {
    return [super count];
}
@end

void §(bump)bump(Counter *counter) {
    counter.count += [counter count];
}
