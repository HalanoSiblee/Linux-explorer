/* rsbench -- times w2k_rgba_resample and reports its peak memory:
 *   rsbench <src w> <src h> <dst w> <dst h> <method 0-3>
 * Build: gcc -O2 -Iinclude $(pkg-config --cflags xft freetype2) -o rsbench \
 *        tools/rsbench.c lib/resample.c -lm */
#include "w2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
static double now(void){struct timeval t; gettimeofday(&t,NULL); return t.tv_sec+t.tv_usec/1e6;}
int main(int argc,char**argv){
    int sw=atoi(argv[1]),sh=atoi(argv[2]),dw=atoi(argv[3]),dh=atoi(argv[4]),m=atoi(argv[5]);
    unsigned char *src=malloc((size_t)sw*sh*4); for(size_t i=0;i<(size_t)sw*sh*4;i++) src[i]=(unsigned char)(i*31+ (i>>10));
    double t0=now(); unsigned char *o=w2k_rgba_resample(src,sw,sh,dw,dh,m); double t1=now();
    unsigned long sum=0; for(size_t i=0;i<(size_t)dw*dh*4;i+=97) sum+=o[i];
    struct rusage ru; getrusage(RUSAGE_SELF,&ru);
    printf("%dx%d -> %dx%d method %d: %.3f s, peak %ld MB (checksum %lu)\n",sw,sh,dw,dh,m,t1-t0,ru.ru_maxrss/1024,sum);
    free(o); free(src); return 0;
}
