---
issues: [clice#1111]
---

# Declarations are public

The generated declarations go under a `public:` label when the class body ends in another section

A class whose body ends in `private:` or `protected:` gains the label before the declarations, so they are not private.
