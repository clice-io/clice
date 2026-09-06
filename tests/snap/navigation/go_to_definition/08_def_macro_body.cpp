/// # Macro body navigation
///
/// - status: supported
/// - verify: server
///
/// A token written inside a macro body has no meaning until an expansion
/// assigns one, so navigation on it yields nothing, while the invocation token
/// always resolves to the macro being expanded

#define DEFINE_COUNTER int §(body_token)counter = 0

§(invocation)DEFINE_COUNTER;
