#include "common.h"
static bool number(const char *text,long long *out) {
    bool negative=*text=='-';if(negative)text++;
    bool valid;uint32_t n=command_number(text,&valid);
    if(!valid || n>(negative?2147483648U:2147483647U))return false;
    *out=negative?-(long long)n:(long long)n;return true;
}
static int run(int argc,char **argv) {
    long long a,b,r;
    if(argc!=4 || !number(argv[1],&a) || !number(argv[3],&b))return command_error("calc","uso: calc entero operador entero (32 bits con signo)");
    if(command_is(argv[2],"+"))r=a+b;
    else if(command_is(argv[2],"-"))r=a-b;
    else if(command_is(argv[2],"*"))r=a*b;
    else if(command_is(argv[2],"/") && b)r=a/b;
    else if(command_is(argv[2],"%") && b)r=a%b;
    else return command_error("calc","operador invalido o division por cero");
    if(r<(-2147483647LL-1) || r>2147483647LL)return command_error("calc","resultado fuera de 32 bits");
    kprintf("%d\n",(int)r);return 0;
}
BK_COMMAND_MAIN(run)
