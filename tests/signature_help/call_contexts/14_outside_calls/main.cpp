int area(int width, int height);

int area(int width, §(definition)int height) {
    return width * height;
}

int total = area(1, 2)§(after_call);

int scaled = area(2, 3) * 2;  // area(4, §(comment)5)
