/// # Template argument and parameter lists
///
/// - status: supported
///
/// Template arguments and template parameters are selected inside their angle
/// brackets before with them

template <class First, class §(param)Second>
struct Pair {
    First first;
    Second second;
};

template <class T>
T identity(T value) {
    return value;
}

Pair<int, §(type_arg)long> pair{1, 2};

int same = identity<§(call_arg)int>(3);
