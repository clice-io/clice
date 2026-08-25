/// # Type Information
///
/// ## Concept constraints — the constraint behind a parameter or `auto` placeholder
///
/// - status: partial
/// - order: 13
///
/// The constrained-parameter and concept-reference cards carry the
/// constraint, but hovering the placeholder of a constrained `Addable auto`
/// variable shows only the deduced type — the constraint is dropped.

namespace concept_constraints {

template <typename T>
concept Addable = requires(T a) { a + a; };

template <§(01_concept_name)Addable §(02_param_name)U>
void sum(U a, U b);

auto flag = §(03_concept_ref)Addable<int>;

Addable §(04_constrained_auto)auto §(05_constrained_var)total = 1;

}
