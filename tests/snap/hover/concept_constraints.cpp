/// # Type Information
///
/// ## Concept constraints — the concept behind a constrained parameter
///
/// - status: supported
/// - order: 13
///
/// A constrained template parameter, the concept it names, and a concept
/// reference; probing what each reports about the constraint.

namespace concept_constraints {

template <typename T>
concept Addable = requires(T a) { a + a; };

template <§(01_concept_name)Addable §(02_param_name)U>
void sum(U a, U b);

auto flag = §(03_concept_ref)Addable<int>;

}
