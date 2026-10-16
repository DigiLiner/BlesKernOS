#include <errno.h>
#include <math.h>

double fabs(double x){return x<0?-x:x;}
double floor(double x){long i=(long)x;return(double)i>x?(double)(i-1):(double)i;}
double ceil(double x){long i=(long)x;return(double)i<x?(double)(i+1):(double)i;}
double fmod(double x,double y){if(y==0){errno=EDOM;return NAN;}return x-(double)((long)(x/y))*y;}
double modf(double x,double*i){double whole=x<0?ceil(x):floor(x);if(i)*i=whole;return x-whole;}
double ldexp(double x,int e){if(e>0)while(e--)x*=2.0;else while(e++)x*=0.5;return x;}
double frexp(double x,int*e){int n=0;if(x==0){if(e)*e=0;return 0;}double a=fabs(x);while(a>=1){a*=0.5;n++;}while(a<0.5){a*=2;n--;}if(e)*e=n;return x<0?-a:a;}
double sqrt(double x) {
    double result;
    if (x < 0) { errno = EDOM; return NAN; }
    /* i386 already requires x87. FSQRT also handles subnormals, infinity,
     * NaN and signed zero, unlike a fixed number of Newton iterations. */
    __asm__ ("fsqrt" : "=t" (result) : "0" (x));
    return result;
}
static double wrap(double x){while(x>M_PI)x-=2*M_PI;while(x<-M_PI)x+=2*M_PI;return x;}
double sin(double x){x=wrap(x);double x2=x*x;return x*(1-x2/6+x2*x2/120-x2*x2*x2/5040+x2*x2*x2*x2/362880);}
double cos(double x){x=wrap(x);double x2=x*x;return 1-x2/2+x2*x2/24-x2*x2*x2/720+x2*x2*x2*x2/40320;}
double tan(double x){double c=cos(x);if(fabs(c)<1e-12)return x<0?-HUGE_VAL:HUGE_VAL;return sin(x)/c;}
double atan(double x){int inverse=fabs(x)>1;double z=inverse?1/x:x,z2=z*z;double r=z*(1-z2/3+z2*z2/5-z2*z2*z2/7+z2*z2*z2*z2/9-z2*z2*z2*z2*z2/11);if(inverse)r=(x<0?-M_PI_2:M_PI_2)-r;return r;}
double atan2(double y,double x){if(x>0)return atan(y/x);if(x<0)return atan(y/x)+(y>=0?M_PI:-M_PI);if(y>0)return M_PI_2;if(y<0)return-M_PI_2;return 0;}
double asin(double x){if(x>1||x<-1){errno=EDOM;return NAN;}return atan2(x,sqrt((1-x)*(1+x)));}
double acos(double x){if(x>1||x<-1){errno=EDOM;return NAN;}return atan2(sqrt((1-x)*(1+x)),x);}
double exp(double x){if(x>709)return HUGE_VAL;if(x<-745)return 0;int n=(int)(x/M_LN2);double r=x-n*M_LN2,term=1,sum=1;for(int i=1;i<22;i++){term*=r/i;sum+=term;}return ldexp(sum,n);}
double log(double x){if(x<=0){errno=EDOM;return x==0?-HUGE_VAL:NAN;}int e=0;double m=frexp(x,&e)*2;e--;double z=(m-1)/(m+1),z2=z*z,term=z,sum=0;for(int k=1;k<30;k+=2){sum+=term/k;term*=z2;}return 2*sum+e*M_LN2;}
double log10(double x){return log(x)/2.30258509299404568402;}
double pow(double b,double e){if(e==0)return 1;if(b==0)return e>0?0:HUGE_VAL;if(b<0){long n=(long)e;if((double)n!=e){errno=EDOM;return NAN;}double r=exp(e*log(-b));return n&1?-r:r;}return exp(e*log(b));}
float fabsf(float x){return(float)fabs(x);}float sqrtf(float x){return(float)sqrt(x);}float sinf(float x){return(float)sin(x);}float cosf(float x){return(float)cos(x);}
