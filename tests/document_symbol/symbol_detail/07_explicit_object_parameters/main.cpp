struct Widget {
    void visit(this Widget& self);
    void visit(this Widget&& self);
    int size() const;
};
