/// # Memberwise constructor
///
/// - status: supported
///
/// A class receives a constructor taking every field in order, scalars by value and other types by const reference

struct Name {};

struct §(record)Record {
    int id;
    const Name name;
    const char* label;
    double* weight;
};
