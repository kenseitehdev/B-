#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t state;
static unsigned pick(unsigned n){state^=state<<13;state^=state>>17;state^=state<<5;return state%n;}
static void expr(unsigned depth){
    if(!depth){printf("%s",(const char *[]){"a","b","c","d","e","f","g","h"}[pick(8)]);return;}
    putchar('(');expr(depth-1);
    unsigned op=pick(4);
    if(op==2)printf("*%u",pick(5)+1);
    else if(op==3){fputs("+id(",stdout);expr(depth-1);putchar(')');}
    else {putchar(op?'-':'+');expr(depth-1);}
    putchar(')');
}
int main(int argc,char **argv){
    if(argc==3&&!strcmp(argv[1],"--stress")){
        unsigned n=(unsigned)strtoul(argv[2],NULL,10);if(!n||n>100000)return 2;
        puts("define proc wide(n){if(n){");
        for(unsigned j=0;j<n;++j)puts("n=n+1;");
        puts("}else{return 2;}return n;}return wide(0)+wide(1);");
        return ferror(stdout)?1:0;
    }
    state=argc>1?(uint32_t)strtoul(argv[1],NULL,10):1;if(!state)state=1;
    fputs("define proc id(n){return n;}\ndefine proc calc(a,b,c,d,e,f,g,h){return ",stdout);expr(4);fputs(";}\n",stdout);
    printf("new int i=0;new int total=0;while(i<%u){if(i%%2){total=total+calc(i,2,3,4,5,6,7,8);}else{total=total-calc(8,7,6,5,4,3,2,i);}i=i+1;}\n",pick(20)+1);
    printf("new Array arr=Array.new(2);Array.set(arr,0,\"seed%u\");Array.set(arr,1,total);new String s=String.concat(\"v:\",string_new(Array.get(arr,0)));println(s);return Array.get(arr,1);\n",pick(10000));
    return ferror(stdout)?1:0;
}
