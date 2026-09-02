/// # Enum underlying types — the enum-base reference keeps its type kind
///
/// - status: supported

using Byte = unsigned char;

enum class Flags : §Byte { A, B };

Flags flags = Flags::§A;
