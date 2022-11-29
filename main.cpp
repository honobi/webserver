#include <iostream>
#include "locker.h"
#include "threadpool.h"
#include <pthread.h>
#include <unistd.h>

using namespace std;

class Request{
public:
    int m_state;
    Request(int x): m_state(x){

    }
};

int main(){
    
    threadpool<Request> pool(1);

    while(1){
        //sleep(1);
        Request* req = new Request(100);
        pool.append(req, 1);
    }
    
    
    
    return 0;
}


