# Temporaries

The class name of a temporary leads to the constructor it calls

`Widget(1)`, `Widget(2, 3)`, `Widget{4}` and `Widget()` call a constructor
by the class's name: go-to-definition, declaration, hover, highlights and
references there answer that constructor, and go-to-type-definition the
class, a qualified name and a template's specialization alike.
