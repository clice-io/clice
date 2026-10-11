namespace garden {

enum class Season { spring, autumn };

struct Bed {
    int width;
    void plant(Season season);
};

void water(Bed& bed);

}
