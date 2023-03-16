#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h> 
#include <list>
#include <exception>

#include "locker.h"
#include "mysql/sql_connection_pool.h"

template<typename T>
class m_priority_queue{
    std::list<T*> high_priority_queue;  //高优先级队列对应的优先级为0，低优先级为1
    std::list<T*> low_priority_queue;

public:
    void push_back(T* request){
        int priority = request->http_priority;
        if(priority == 0)
            high_priority_queue.push_back(request);
        else
            low_priority_queue.push_back(request);
    }
    
    T* front(){
        if(high_priority_queue.size() != 0)
            return high_priority_queue.front();
        return low_priority_queue.front();
    }

    void pop_front(){
        if(high_priority_queue.size() != 0){
            high_priority_queue.pop_front();
            return;
        }
        low_priority_queue.pop_front();
    }
};

template<typename T>
class threadpool{
public:
    //将默认实参放在声明中
    threadpool(int actor_model, connection_pool* connPool,  int thread_number = 8, int max_request = 1000);
    ~threadpool();
    bool append(T* request, int state);
    bool append_p(T* request);

private:

    static void* worker(void*); //静态函数
    void run();

    int m_thread_number;  //线程池中的线程数
    int m_max_requests;   //请求队列允许的最大请求数
    pthread_t* m_threads; //线程池数组，保存线程id
    m_priority_queue<T> m_workqueue;  //请求队列
    locker m_queuelocker;   //保护请求队列的互斥锁
    sem m_tasks;      //任务队列中任务数
    sem m_free;           //任务队列空闲位置数，初始化为请求队列最大任务数
    connection_pool* m_connPool; //数据库连接池
    int m_actor_model;    //事件处理模式

};

template<typename T>
threadpool<T>::threadpool(int actor_model, connection_pool* connPool,  int thread_number, int max_request)
: m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_request), 
m_threads(NULL), m_connPool(connPool), m_free(max_request) {

    if(thread_number <= 0 || max_request <= 0)
        throw std::exception(); 
    
    //创建线程池数组
    m_threads = new pthread_t[m_thread_number]; 
    //new失败时，返回一个空指针，抛出一个类型为bad_alloc的异常

    for(int i = 0; i < m_thread_number; ++i){
        if(pthread_create(&m_threads[i], NULL, worker, this) != 0){
            //构造函数失败，需要释放已分配的资源
            delete[] m_threads; 
            throw std::exception();
        }

        //设置线程分离。线程结束后，资源自动回收
        if(pthread_detach(m_threads[i]) != 0){ 
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
}

//将http请求放进请求队列。该函数适用于Reactor模式。参数state标记该http连接处于的阶段是读阶段还是写阶段
template<typename T>
bool threadpool<T>::append(T* request, int state){

    //获任务队列的一个空位
    m_free.wait();
    m_queuelocker.lock(); 

    //将任务放进任务队列
    m_workqueue.push_back(request);
    request->m_state = state;  //设置http连接状态
    
    //退出临界区并 队列中任务数+1
    m_queuelocker.unlock(); 
    m_tasks.post();  //sem_post会唤醒优先级较高或者较早等待的线程
    return true;

}

//Proactor模式的append。
//Proactor模式不需要state参数：Proactor模式下，工作线程不需要负责IO，也就无所谓读阶段还是写阶段
template<typename T>
bool threadpool<T>::append_p(T* request){ 

    //获取任务队列的一个空位
    m_free.wait();
    m_queuelocker.lock(); 
    
    //将任务放进任务队列
    m_workqueue.push_back(request);

    //退出临界区并 队列中任务数+1
    m_queuelocker.unlock(); 
    m_tasks.post();
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg){

    //通过实参this指针获取创建好的线程池的实例
    threadpool* pool = (threadpool*)arg; 
    pool->run();
}

//工作线程从请求队列中取出某个任务进行处理
//如果是Reactor模式，需要工作线程负责IO。Proactor模式不需要工作线程负责IO，只处理业务逻辑即可
template<typename T>
void threadpool<T>::run(){  

    while(true){
        
        //任务数P操作
        m_tasks.wait();  
        m_queuelocker.lock(); //进入临界区

        //工作线程作为消费者，消费一个请求
        T* request = m_workqueue.front();
        m_workqueue.pop_front();

        //退出临界区并空闲数+1
        m_queuelocker.unlock(); 
        m_free.post();

        //获取资源失败，进入下一次循环
        if (!request)
            continue;

        //下面消费者消耗一个产品

        /*Reactor模式：主线程(I/O处理单元)只负责监听文件描述上是否有事件发生，然后通知工作线程(逻辑单元)。
        工作线程需要 处理业务逻辑 和I/O（数据的接收和发送）*/
        if (m_actor_model == 1) {

            //对该http请求的处理处于 读阶段：将请求报文读到缓冲区，并解析，然后将响应报文写入写缓冲区
            if (request->m_state == 0) {

                //工作线程负责IO：将接收到的数据放到读缓冲区
                int res = request->read_once();

                request->have_io = 1; //标记该连接执行过IO操作

                //IO成功，从数据库连接池中取出一个数连接，然后process解析读缓冲区中的请求报文并将响应报文写入写缓冲区
                if (res) {
                    connectionRAII mysqlcon(&request->mysql, m_connPool); 
                    request->process();
                }

                //IO失败，设置标记timer_flag，标记该连接应该被关闭
                else
                    request->shoule_close = 1;
            }

            //对该http请求的处理处于 写阶段：将写缓冲区中的响应报文发送给浏览器端
            else {

                //工作线程负责IO：将相应报文放进写缓冲区
                int res = request->write();

                //如果是短连接，那么标记该连接应该被关闭
                if(request->if_keep_alive() == false)
                    request->shoule_close = 1;

                request->have_io = 1; //标记该连接执行过IO操作

                //IO失败，设置标记timer_flag，标记该连接应该被关闭
                if (res == 0)
                    request->shoule_close = 1;
            }
        }

        //Proactor模式：所有I/O操作都交给主线程和内核来处理，也就是数据的接收和发送交给主线程
        //工作线程仅仅负责业务逻辑，也就是 解析 读缓冲区的请求报文，然后把响应报文写到 写缓冲区中
        else {
            //先从数据库连接池取出一个连接
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }

    }
}

#endif