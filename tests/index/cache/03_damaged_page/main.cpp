#define ONE(n) int f##n() { return 0; }
#define TEN(n) ONE(n##0) ONE(n##1) ONE(n##2) ONE(n##3) ONE(n##4) ONE(n##5) ONE(n##6) ONE(n##7) ONE(n##8) ONE(n##9)
#define HUNDRED(n) TEN(n##0) TEN(n##1) TEN(n##2) TEN(n##3) TEN(n##4) TEN(n##5) TEN(n##6) TEN(n##7) TEN(n##8) TEN(n##9)
#define THOUSAND(n) HUNDRED(n##0) HUNDRED(n##1) HUNDRED(n##2) HUNDRED(n##3) HUNDRED(n##4) HUNDRED(n##5) HUNDRED(n##6) HUNDRED(n##7) HUNDRED(n##8) HUNDRED(n##9)
THOUSAND(1)
int main() { return f1000(); }
