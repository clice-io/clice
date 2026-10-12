#pragma once

struct Shape {
    Shape();
    virtual ~Shape();
    virtual double area() const;
};

struct Circle : Shape {
    Circle(double r);
    ~Circle() override;
    double area() const override;
    double radius;
};
