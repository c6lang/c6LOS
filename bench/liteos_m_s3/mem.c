void *memcpy(void *d, const void *s, unsigned int n) { unsigned char *a=d; const unsigned char *b=s; while(n--) *a++=*b++; return d; }
void *memset(void *d, int c, unsigned int n) { unsigned char *a=d; while(n--) *a++=(unsigned char)c; return d; }
int printf(const char *fmt, ...) { (void)fmt; return 0; }
