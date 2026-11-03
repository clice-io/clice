struct Matcher {
    Matcher bind(const char* name) const;
};

Matcher near(const double& value, const double& threshold);
Matcher literal(Matcher inner);
Matcher any(Matcher first, Matcher second);
