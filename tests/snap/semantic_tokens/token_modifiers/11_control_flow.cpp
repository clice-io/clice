/// # Control flow
///
/// - status: supported
///
/// Keywords that transfer control carry the control-flow modifier; other
/// keywords, `default` in either role included, stay plain

int classify(int value) {
    §switch(value) {
        §case 0: §return 0;
        §default: §break;
    }
    §for(int i = 0; i < value; ++i) {
        §if(i == 3) {
            §continue;
        } §else {
            §goto done;
        }
    }
done:
    §while(value > 0) {
        value -= 1;
    }
    §do {
        value += 1;
    } §while(value < 0);
    §try {
        §throw value;
    } §catch(int) {}
    return §static_cast<int>(value);
}

struct Widget {
    §virtual ~Widget() = §default;
};
