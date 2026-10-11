struct Config {
    int width;
    int height;
};

int process_data(const Config& cfg)
{
    int area = cfg.width * cfg.height;
    return area;
}

static int
scale(const Config& cfg,
      int factor)
{
    return cfg.width * factor;
}

class Renderer
    : public Config
{
    int frames;
};
