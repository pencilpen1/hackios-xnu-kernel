/* Runs the exact shader IR through host LLVM codegen, with strict texture
 * callbacks. A CPU oracle validates values; this is not a GPU performance test. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {unsigned w,h;float *pixels;unsigned reads,writes;} texture;
typedef struct {uint32_t w,h,r,axis,edge,pad[3];float weight[513];} parameters;
void shader_call(unsigned,void*,void*,void*,unsigned,unsigned);
float effect_read(texture *t,unsigned x,unsigned y,unsigned c){assert(x<t->w&&y<t->h&&c<4);t->reads++;return t->pixels[(y*t->w+x)*4+c];}
void effect_write(texture *t,unsigned x,unsigned y,float r,float g,float b,float a){assert(x<t->w&&y<t->h);float v[]={r,g,b,a};for(unsigned c=0;c<4;c++)t->pixels[(y*t->w+x)*4+c]=v[c];t->writes++;}
static texture make(unsigned w,unsigned h){texture t={w,h,calloc((size_t)w*h*4,sizeof(float)),0,0};assert(t.pixels);return t;}
static void run(unsigned kind,texture *a,texture *b,void *p){for(unsigned y=0;y<(b->h+7)/8*8;y++)for(unsigned x=0;x<(b->w+7)/8*8;x++)shader_call(kind,a,b,p,x,y);assert(b->writes==b->w*b->h);}
int main(void){
 unsigned shapes[][4]={{17,11,9,7},{3,2,19,13},{1,1,8,9},{31,17,31,17}};
 for(unsigned n=0;n<4;n++){
  unsigned *d=shapes[n];texture a=make(d[0],d[1]),b=make(d[2],d[3]);
  for(unsigned y=0;y<a.h;y++)for(unsigned x=0;x<a.w;x++)for(unsigned c=0;c<4;c++)a.pixels[(y*a.w+x)*4+c]=(float)((x*7+y*13+c*17)%113)/113;
  run(1,&a,&b,d);
  for(unsigned y=0;y<b.h;y++)for(unsigned x=0;x<b.w;x++)for(unsigned c=0;c<4;c++){
   double sx=fmax(0,((double)x+.5)*a.w/b.w-.5),sy=fmax(0,((double)y+.5)*a.h/b.h-.5);
   unsigned x0=(unsigned)sx,y0=(unsigned)sy,x1=x0+1<a.w?x0+1:x0,y1=y0+1<a.h?y0+1:y0;
   double top=a.pixels[(y0*a.w+x0)*4+c]*(1-(sx-x0))+a.pixels[(y0*a.w+x1)*4+c]*(sx-x0);
   double bot=a.pixels[(y1*a.w+x0)*4+c]*(1-(sx-x0))+a.pixels[(y1*a.w+x1)*4+c]*(sx-x0);
   assert(fabs(b.pixels[(y*b.w+x)*4+c]-(top*(1-(sy-y0))+bot*(sy-y0)))<0.0001);
  }
  free(a.pixels);free(b.pixels);
 }
 for(unsigned edge=0;edge<2;edge++)for(unsigned radius=0;radius<=6;radius+=3){
  texture a=make(9,7),tmp=make(9,7),b=make(9,7);parameters p={.w=9,.h=7,.r=radius,.edge=edge};
  double sum=0;for(int i=-(int)radius;i<=(int)radius;i++){p.weight[i+(int)radius]=exp(-(double)(i*i)/2);sum+=p.weight[i+(int)radius];}
  for(unsigned i=0;i<2*radius+1;i++)p.weight[i]/=(float)sum;
  for(unsigned y=0;y<a.h;y++)for(unsigned x=0;x<a.w;x++)for(unsigned c=0;c<4;c++)a.pixels[(y*a.w+x)*4+c]=(float)((x*3+y*7+c)%31)/31;
  run(0,&a,&tmp,&p);p.axis=1;run(0,&tmp,&b,&p);
  for(unsigned y=0;y<b.h;y++)for(unsigned x=0;x<b.w;x++)for(unsigned c=0;c<4;c++){
   double value=0;
   for(int dy=-(int)radius;dy<=(int)radius;dy++)for(int dx=-(int)radius;dx<=(int)radius;dx++){
    int xx=(int)x+dx,yy=(int)y+dy;
    if(!edge&&(xx<0||yy<0||xx>=(int)a.w||yy>=(int)a.h))continue;
    if(xx<0)xx=0;if(yy<0)yy=0;if(xx>=(int)a.w)xx=a.w-1;if(yy>=(int)a.h)yy=a.h-1;
    value+=a.pixels[(yy*a.w+xx)*4+c]*p.weight[dx+(int)radius]*p.weight[dy+(int)radius];
   }
   assert(fabs(value-b.pixels[(y*b.w+x)*4+c])<0.0001);
  }
  free(a.pixels);free(tmp.pixels);free(b.pixels);
 }
 puts("PASS exact AIR IR: bilinear up/down/native/1px scaling, separable Gaussian zero/clamp edges, padded dispatch bounds and CPU oracle");
}
