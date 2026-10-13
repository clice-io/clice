void draw(int aParam);
void fill(char* pBuffer, int size);

void use() {
    int param = 3;
    char buffer[4];
    draw(param);
    fill(buffer, 4);
}
