#ifndef THREADPOOL_H
#define THREADPOOL_H


#include "locker.h"
#include <pthread.h> //linux多线程编程的接口
#include <list>


template<typename T>
class threadpool{
public:
    
    threadpool(int actor_model, /*connection_pool* connPool, */ int thread_number = 8, int max_request = 1000);
    //将默认实参放在声明中
    ~threadpool();
    bool append(T* request, int state);
    bool append_p(T* request);

private:

    static void* worker(void*); //静态函数
    void run();
    //worker函数并不是画蛇添足，是因为pthread_create的第三个参数需要一个静态的成员函数。
    //因为非静态成员函数第一个参数是一个隐式的this指针，与create函数第三个参数要求的void*不匹配，就无法调用create函数。所以不能直接用非static成员函数创建线程
    
    int m_thread_number;  //线程池中的线程数
    int m_max_requests;   //请求队列允许的最大请求数
    pthread_t* m_threads; //线程池数组。 感觉之后这里可以换成一个vector<pthread_t>，让STL自己管理内存
    std::list<T*> m_workqueue;  //请求队列。  不知道为什么请求队列要叫workqueue 
    locker m_queuelocker;   //保护请求队列的互斥锁
    sem m_queuestat;      //请求队列中请求数的信号量
    //connection_pool* m_connPool; //数据库，还没实现，先写在注释里
    int m_actor_model;    //事件处理模式

};

template<typename T>
threadpool<T>::threadpool(int actor_model, /*connection_pool* connPool, */ int thread_number, int max_request)
: m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_request), 
m_threads(NULL)/*, m_connPool(connPool)*/ {

    if(thread_number <= 0 || max_request <= 0)
        throw std::exception(); 
    
    m_threads = new pthread_t[m_thread_number];  //new一个保存线程id的数组作为线程池

    if(!m_threads) //new失败时，会一个空指针。
        throw std::exception();
    //当自由空间被耗尽时，new表达式就会失败，并抛出一个类型为bad_alloc的异常，所以我觉得这一步判断是画蛇添足

    for(int i = 0; i < m_thread_number; ++i){

        if(pthread_create(&m_threads[i], NULL, worker, this) != 0){
        //int pthread_create(pthread_t* thread,const pthread_attr_t* attr, void*(*start_routine)(void*), void* arg);
            //第一个参数是 类型为指针的传出参数，线程创建好后，返回线程id。
            //第二个参数 NULL表示使用默认属性
            //第三个参数 指定新线程将运行的函数，是一个返回值void*，参数void*的函数指针。
                //这个函数不能是类的非静态成员函数，因为每个非static成员函数第一个参数都是一个隐式的this指针，那么参数类型为this，而这里要求一个void的参数。
            //第四个参数：上面函数的参数，void* 类型
            delete[] m_threads; //构造函数失败，需要释放已分配的资源
            throw std::exception();
        }

        if(pthread_detach(m_threads[i]) != 0){ //设置线程分离。线程结束后，资源自动回收
            delete[] m_threads;
            throw std::exception();
        }

    }

}

template<typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
}

template<typename T>
bool threadpool<T>::append(T* request, int state){

    m_queuelocker.lock(); //对请求队列上锁

    if(m_workqueue.size() >= m_max_requests){
        m_queuelocker.unlock();  //解锁
        return false;
    }

    request->m_state = state;  //应该是设置状态吧。 这里就要求我们的模板实参T要有成员 m_state了
    m_workqueue.push_back(request);
    m_queuelocker.unlock(); //解锁
    m_queuestat.post();  //对代表 请求队列中的请求数的信号量 进行V操作
    return true;

}

template<typename T>
bool threadpool<T>::append_p(T* request){   //只比append少了一个 设置状态的步骤，其他一样。我猜可能是post请求
    m_queuelocker.lock(); 
    if(m_workqueue.size() >= m_max_requests){
        m_queuelocker.unlock();  
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock(); 
    m_queuestat.post();  //在将请求放进请求队列之后，才post信号量
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg){

    threadpool* pool = (threadpool*)arg; //把void*显示转换成threadpool类型（这里实参应该是一个threadpool的this指针）
    pool->run();
    return pool; //在这里return一个pool，不知道有什么用
    //void*类型的返回值与void一样，都是可以不写return的
}

template<typename T>
void threadpool<T>::run(){    //run函数暂时没法写，因为涉及到具体业务逻辑，下面仅为测试用

    while(true){
        
        m_queuestat.wait();  //P操作，对代表 请求队列中请求数的信号量 进行P操作。

        m_queuelocker.lock(); //请求队列上锁

        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }

        /*
            信号量仅起到标志请求队列中是否有请求，当队列为空时，信号量为0，会阻塞，减少cpu消耗。
            由于wait与lock是两步操作，并非原子操作，wait成功后，还需要获取请求队列的锁，所以还需要等待。
            当获取锁之后，由于请求队列中 请求可能已经被消耗完，所以需要判断队列是否为空
        */
        /*信号量的值并不能准确的代表队列中的请求数。
            在append函数中，我们是先把请求放进队列，释放了锁，然后操post的信号量。
            在run函数中，我们是先wait的信号量，然后才加锁，尝试取出请求
            也就是说，在一些时刻，信号量的值并不是准确的，并不真正代表请求队列中的请求数。
            我们只能用信号量做一些粗略的判断，比如判断队列中是否有请求，没有就阻塞住
        */ 

       std::cout << "从请求队列取出请求" << std::endl;

       m_queuelocker.unlock();

    }
}

#endif