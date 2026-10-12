// Bases as everyday code writes them: a class template's specialization,
// whose parameter types are substituted and whose default arguments stay
// behind; a base whose pure virtual method is private, as the non-virtual
// interface idiom writes it; a base whose only pure virtual method is its
// destructor, which needs no override.

template <class T>
struct Source {
    virtual T get() const = 0;
    virtual void put(T value, int times = 1) = 0;
    virtual ~Source() = 0;
};

struct §(specialization)Numbers : Source<int> {};

class Task {
public:
    void run();

private:
    virtual void step() = 0;
};

class §(private_virtual)Download : public Task {};

struct Interface {
    virtual ~Interface() = 0;
};

struct §(destructor_only)Concrete : Interface {};
