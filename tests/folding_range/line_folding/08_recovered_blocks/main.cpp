// Recovering from the unknown `missing_list`, clang closes the class at the
// constructor's brace while the text pairs it with `};`: of the folds that
// cross by lines, the outer one stays.
namespace outer
{
template <typename T>
class Holder
{
public:
    Holder(missing_list<T> values, int flags = 0)
    { load(values.begin(), flags); }

    int size() const;
};

int helper();
}
