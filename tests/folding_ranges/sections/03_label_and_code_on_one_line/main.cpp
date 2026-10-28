// An access section whose label shares its line with code folds from
// right after the label, around the folds that code starts on the line.
class Widget {
public: void draw() {
        int a = 1;
        a += 1;
    }
    void resize();
private:
    int width;
    int height;
};

struct Annotated {
public: /* drawing */ void draw() {
        int a = 1;
        a += 1;
    }
    void resize();
private:
    int width;
};
