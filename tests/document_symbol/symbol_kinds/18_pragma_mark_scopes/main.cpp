// A mark without a dash is an entry of its own; a section inside a class
// groups its members, and one without a name still groups; a mark in an
// inactive branch is not there.

#pragma mark Helpers

int helper();

class Widget {
public:
#pragma mark - Drawing
    void draw();
    void paint();

#pragma mark -
    int width;
};

#if 0
#pragma mark - Disabled
#endif

int after();
