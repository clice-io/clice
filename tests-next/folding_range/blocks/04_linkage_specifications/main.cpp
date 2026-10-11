extern "C" {

int c_open(const char* path);
int c_close(int handle);

}

#ifdef __cplusplus
extern "C" {
#endif

int c_read(int handle);
int c_write(int handle);

#ifdef __cplusplus
}
#endif
