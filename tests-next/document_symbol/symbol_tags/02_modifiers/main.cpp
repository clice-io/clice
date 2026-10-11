class Base {
public:
    virtual void render() = 0;

protected:
    static int instances();

private:
    int id;
};
