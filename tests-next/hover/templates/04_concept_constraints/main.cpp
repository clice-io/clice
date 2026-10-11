namespace concept_constraints {

template <typename T>
concept Addable = requires(T a) { a + a; };

template <§(concept_name)Addable §(param_name)U>
void sum(U a, U b);

auto flag = §(concept_ref)Addable<int>;

Addable §(constrained_auto)auto §(constrained_var)total = 1;

}
