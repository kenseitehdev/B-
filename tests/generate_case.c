#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This test-only evaluator is independent of compiler IR, optimization and
 * native lowering. It supplies expected results after retiring the C backend. */
typedef struct { unsigned op, argument; size_t left, right; long long factor; } Expr;
static Expr nodes[128];
static size_t node_count;
static uint32_t state;
static unsigned pick(unsigned n){state^=state<<13;state^=state>>17;state^=state<<5;return state%n;}
static size_t expression(unsigned depth){
    size_t id=node_count++;
    if(!depth){nodes[id].op=8;nodes[id].argument=pick(8);return id;}
    nodes[id].left=expression(depth-1);nodes[id].op=pick(8);
    if(nodes[id].op==2||nodes[id].op==6||nodes[id].op==7)nodes[id].factor=pick(5)+1;
    else nodes[id].right=expression(depth-1);
    return id;
}
static void print_expr(size_t id){
    const Expr *e=&nodes[id];
    if(e->op==8){putchar((int)('a'+e->argument));return;}
    putchar('(');print_expr(e->left);
    if(e->op==2||e->op==6||e->op==7)printf("%c%lld",e->op==2?'*':e->op==6?'/':'%',e->factor);
    else if(e->op==3){fputs("+id(",stdout);print_expr(e->right);putchar(')');}
    else {fputs(e->op==4?"==":e->op==5?"<":e->op==1?"-":"+",stdout);print_expr(e->right);}
    putchar(')');
}
static long long evaluate(size_t id,const long long arguments[8]){
    const Expr *e=&nodes[id];
    if(e->op==8)return arguments[e->argument];
    long long left=evaluate(e->left,arguments);
    if(e->op==2)return left*e->factor;
    if(e->op==6)return left/e->factor;
    if(e->op==7)return left%e->factor;
    long long right=evaluate(e->right,arguments);
    return e->op==4?left==right:e->op==5?left<right:e->op==1?left-right:left+right;
}
int main(int argc,char **argv){
    if(argc==3&&!strcmp(argv[1],"--stress")){
        unsigned n=(unsigned)strtoul(argv[2],NULL,10);if(!n||n>100000)return 2;
        puts("define proc wide(n){if(n){");
        for(unsigned j=0;j<n;++j)puts("n=n+1;");
        puts("}else{return 2;}return n;}define proc main(){return wide(0)+wide(1);}");
        return ferror(stdout)?1:0;
    }
    int expected=argc>2&&!strcmp(argv[1],"--expected");
    unsigned depth_limit=4;
    if(argc>=4&&!strcmp(argv[argc-2],"--depth")){depth_limit=(unsigned)strtoul(argv[argc-1],NULL,10);if(depth_limit>6)return 2;}
    state=argc>1?(uint32_t)strtoul(argv[expected?2:1],NULL,10):1;if(!state)state=1;
    size_t root=expression(depth_limit);
    unsigned limit=pick(20)+1,first=pick(256),second=pick(256),last=pick(256),depth=pick(12);
    if(expected){
        long long total=0;
        for(unsigned i=0;i<limit;++i){
            long long a[]={i,2,3,4,5,6,7,8},b[]={8,7,6,5,4,3,2,i};
            total+=i%2?evaluate(root,a):-evaluate(root,b);
        }
        printf("%lld\n",total+first+second+last+(long long)depth*(depth+1)/2);
    }else{
        fputs("define proc id(n){return n;}\ndefine proc calc(a,b,c,d,e,f,g,h){return ",stdout);print_expr(root);fputs(";}\n",stdout);
        puts("define proc walk(n){if(n<=0){return 0;}return n+walk(n-1);}");
        puts("define proc mutate(p,n){p[1]=n;return p[0]+p[1];}");
        printf("define proc main(){new byte i=0;new byte total=0;while(i<%u){if(i%%2){total=total+calc(i,2,3,4,5,6,7,8);}else{total=total-calc(8,7,6,5,4,3,2,i);}i=i+1;}\n",limit);
        printf("new ptr p=malloc 2;p[0]=%u;p[1]=0;new ptr alias=p;new byte bytes=mutate(alias,%u);p=remalloc p 8;p[7]=%u;bytes=bytes+p[7];free p;delete p;return total+bytes+walk(%u);}\n",first,second,last,depth);
    }
    return ferror(stdout)?1:0;
}
