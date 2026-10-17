---
issues: [clangd#1558]
---

# Calls through overridden methods

Find references on an override includes the calls made through the methods it overrides, which may dispatch to it

`shape.area()` on a `Shape&` may run `Circle::area`, so it is among the
references of the override. A call through `Circle` never runs
`Shape::area`, so it is not among the references of the method overridden;
nor is `Shape::area()` in the override's own body, which names the method
overridden, or `circle.Shape::scale(factor)` in a template, which names one
of the overloads overridden before any instantiation picks it.
