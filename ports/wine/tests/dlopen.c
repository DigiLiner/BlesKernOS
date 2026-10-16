#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){printf("[DLOPEN] linea=%d error=%s FAIL\n",__LINE__,dlerror());return 1;}}while(0)
static int (*value)(void);
static void (*set)(int);
static void *worker(void *unused) {
    if(value()!=73)return (void *)1;
    set(99);return value()==99?NULL:(void *)2;
}
int main(void) {
    for(unsigned i=0;i<12;i++) {
        void *a=dlopen("/WINEGATE/lib/libgate.so",RTLD_NOW|RTLD_LOCAL);CHECK(a);
        void *b=dlopen("/WINEGATE/lib/./libgate.so",RTLD_NOW);CHECK(b==a);
        value=dlsym(a,"gate_value");set=dlsym(a,"gate_set");CHECK(value && set && value()==73);
        Dl_info info;CHECK(dladdr(value,&info) && strstr(info.dli_fname,"libgate.so"));
        set(81);pthread_t thread;void *result;
        CHECK(!pthread_create(&thread,NULL,worker,NULL) && !pthread_join(thread,&result) && result==NULL);
        CHECK(value()==81 && dlclose(a)==0 && value()==81 && dlclose(b)==0);
    }
    puts("[DLOPEN] identidad, referencias, TLS por hilo y recarga PASS");return 0;
}
