# C++20 rewritten operators

Rewritten comparisons navigate to the operator that implements them

For a comparison synthesized by the C++20 rewrite rules, go-to-definition
on the written operator reaches the operator that actually implements it:
`!=` reaches `operator==`, and `>` reaches `operator<=>`.
