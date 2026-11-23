class §(file)File {
public:
    File(int fd) : fd(fd) {}

    int descriptor() const {
        return fd;
    }

private:
    int fd;
};

class §(handle)Handle {
public:
    Handle(int kind) : kind(kind) {}

protected:
    int kind;
};
