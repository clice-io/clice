struct Employee {
    int salary;
};

int payroll(Employee one, int count) {
    auto pay = [](auto& §(parameter)e) {
        return e.salary;
    };
    return pay(one) + pay(count);
}
