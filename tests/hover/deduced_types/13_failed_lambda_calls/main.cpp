struct Employee {
    int salary;
};

int payroll(Employee one, int count) {
    auto pay = [](auto& §(parameter)e) {
        return e.salary;
    };
    return pay(one) + pay(count);
}

int failed(int count) {
    auto pay = [](auto& §(only_failed)e) {
        return e.salary;
    };
    return pay(count);
}
