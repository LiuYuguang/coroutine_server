#include <stdio.h>
#include "coroutine.h"

int n = 1000;

void func(schedule* S,void* arg){
    int i,j;
    j = n--;
    if(j){
        coroutine_new(S,func,NULL);    
    }
    for(i=0;i<1000;i++){
        // printf("i%d j%d %s\n",i,j,__FUNCTION__);
        coroutine_yield(S);
    }
    // printf("%s exit\n",__FUNCTION__);
}

int main(){
    schedule *S = coroutine_open();
    coroutine_new(S,func,NULL);
    coroutine_loop(S);
    coroutine_close(S);
    return 0;
}

// real    0m0.908s
// user    0m0.599s
// sys     0m0.310s