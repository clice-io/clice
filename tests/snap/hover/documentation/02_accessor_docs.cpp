/// # Synthesized accessor docs
///
/// - status: supported
///
/// Trivial getters/setters get a generated one-line description
///
/// A trivial getter or setter with no comment of its own gets a synthesized
/// "Trivial accessor/setter for `field`." line in its hover card.

namespace generated_docs {
struct Meter {
    long reading;
    long §(01_getter)getReading() { return reading; }
    void §(02_setter)setReading(long next) { reading = next; }
};
}
