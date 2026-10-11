// A use in a branch the preprocessor skips names nothing.

int §(shown)shown() {
    return 1;
}

#if 0
int hidden = shown();
#else
int visible = shown();
#endif
