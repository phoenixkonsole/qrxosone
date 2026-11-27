#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
static unsigned fb(float f){unsigned u;memcpy(&u,&f,4);return u;}
static unsigned long long db(double d){unsigned long long u;memcpy(&u,&d,8);return u;}
volatile float fv[] = {0.0f,-0.0f,1.0f,-1.5f,3.14159265f,1e-40f,1e38f,-1e38f,0.1f,2147483648.0f,-2147483904.0f,4294967296.0f,1e20f,-7.25f,16777217.0f, 0.0f/0.0f, 1.0f/0.0f, -1.0f/0.0f};
volatile double dv[] = {0.0,-0.0,1.0,-1.5,3.141592653589793,1e-310,1e308,-1e308,0.1,2147483648.0,-2147483649.0,4294967296.0,1e20,-7.25,9007199254740993.0, 0.0/0.0, 1.0/0.0, -1.0/0.0};
volatile long long lv[] = {0,1,-1,2147483647LL,-2147483648LL,4294967295LL,9007199254740993LL,-9223372036854775807LL-1,9223372036854775807LL};
int main(void){
  unsigned long h=5381; int n=sizeof fv/sizeof fv[0], m=sizeof lv/sizeof lv[0];
#define H(x) do{ h=h*33+(unsigned long)(x); }while(0)
  for(int i=0;i<n;i++){ float a=fv[i]; double c=dv[i];
    H(fb(sqrtf(a<0?-a:a))); H(db(sqrt(c<0?-c:c))); H((int)a==0x7fffffff); 
    H(db((double)a)); H(fb((float)c));
    if(a==a && a<2e9f && a>-2e9f){ H((int)a); H((long long)a); } 
    if(a==a && a>=0 && a<4e9f){ H((unsigned)a); H((unsigned long long)a); }
    if(c==c && c<2e9 && c>-2e9){ H((int)c); H((long long)c); }
    if(c==c && c>=0 && c<4e9){ H((unsigned)c); H((unsigned long long)c); }
    for(int j=0;j<n;j++){ float b=fv[j]; double d=dv[j];
      H(fb(a+b)); H(fb(a-b)); H(fb(a*b)); H(fb(a/b));
      H(db(c+d)); H(db(c-d)); H(db(c*d)); H(db(c/d));
      H(a<b); H(a<=b); H(a>b); H(a>=b); H(a==b); H(a!=b);
      H(c<d); H(c<=d); H(c>d); H(c>=d); H(c==d); H(c!=d);
      H(isunordered(a,b)); H(isunordered(c,d));
    }}
  for(int i=0;i<m;i++){ long long x=lv[i]; H(fb((float)x)); H(db((double)x)); H(fb((float)(unsigned long long)x)); H(db((double)(unsigned long long)x)); H(fb((float)(int)x)); H(db((double)(int)x)); H(fb((float)(unsigned)x)); H(db((double)(unsigned)x)); }
  printf("FPHASH %lx\n", h);
  return 0;
}
