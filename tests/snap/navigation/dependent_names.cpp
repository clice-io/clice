/// - verify: server

template <typename T>
int inspect_dependent(T& value) {
    typename T::§(dependent_type)result made = value.§(dependent_call)make();
    return made + value.§(dependent_field)count;
}
