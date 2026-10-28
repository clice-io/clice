export module app:measure;

export namespace app::measures {

double measure_area(double radius);

double ring(double radius) {
    return perim§(same_partition)(radius);
}

double perimeter(double radius);

}  // namespace app::measures
