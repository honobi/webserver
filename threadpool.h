//线程池之所以只有.h头文件 而不采用.h声明+.cpp定义，是因为：线程池是一个类模板，函数模板和类模板头文件通常既包括声明也包括定义
/*原因：
普通函数：当我们调用一个函数时，编译器只需要掌握函数的声明
普通类：当我们使用一个类类型的对象时，类定义必须是可用的，但成员函数的定义不必已经出现
    因此，我们将类定义和函数声明放在头文件中，而普通函数和类的成员函数的定义放在源文件中。
模板：当编译器遇到一个模板定义时，它并不生成代码（而普通类和函数是生成代码的），模板直到实例化时才会生成代码
    为了实例化，编译器需要掌握函数模板或类模板成员函数的定义。因此，模板的头文件通常既包括声明也包括定义

C++采用了分离式编译的方法，.h头文件仅仅是在预处理阶段进行展开的，真正进行的是对.cpp文件的编译。
编译器在编译时，看到模板并不会进行任何操作，而是在模板实际使用时（实例化），才会进行代码生成。
如果模板定义放在.h头文件中，模板实现放在.cpp文件中，编译时可以看到模板的声明，但找不到定义，
因此会成为外部符号，而在链接时，必然无法找到模板的实现（该外部符号的对应符号），导致链接失败。
而如果模板定义在.h头文件中，则可以在编译时就找到模板的定义，进行代码生成。

简单来说：如果定义放在.cpp文件，那么由于模板的性质，在编译期间没有生成代码，只能找到.h文件中的模板声明，但找不到.cpp文件中的定义
*/


#ifndef THREADPOOL_H
#define THREADPOOL_H


#include <pthread.h> //linux多线程编程的接口
#include <list>
#include <exception>


#include "locker.h"
#include "CGImysql/sql_connection_pool.h"



template<typename T>
class threadpool{
public:
    
    threadpool(int actor_model, connection_pool* connPool,  int thread_number = 8, int max_request = 1000);
    //将默认实参放在声明中
    ~threadpool();
    bool append(T* request, int state);
    bool append_p(T* request);

private:

    static void* worker(void*); //静态函数
    void run();
    //worker函数并不是画蛇添足，是因为pthread_create的第三个参数需要一个静态的成员函数。
    //因为非静态成员函数第一个参数是一个隐式的this指针，与create函数第三个参数要求的void*不匹配，就无法调用create函数。所以不能直接用非static成员函数创建线程
    //但是我认为run函数和worker函数是可以合并为一个静态函数的，把run函数的内容放进worker里就可以

    int m_thread_number;  //线程池中的线程数
    int m_max_requests;   //请求队列允许的最大请求数
    pthread_t* m_threads; //线程池数组。 感觉之后这里可以换成一个vector<pthread_t>，让STL自己管理内存
    std::list<T*> m_workqueue;  //请求队列。  不理解为什么请求队列偏要叫workqueue 
    locker m_queuelocker;   //保护请求队列的互斥锁
    sem m_queuestat;      //请求队列中请求数的信号量
    connection_pool* m_connPool; //数据库，还没实现，先写在注释里
    int m_actor_model;    //事件处理模式

};

template<typename T>
threadpool<T>::threadpool(int actor_model, connection_pool* connPool,  int thread_number, int max_request)
: m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_request), 
m_threads(NULL), m_connPool(connPool) {

    if(thread_number <= 0 || max_request <= 0)
        throw std::exception(); 
    
    m_threads = new pthread_t[m_thread_number];  //new一个保存线程id的数组作为线程池

    if(!m_threads) //new失败时，返回一个空指针。
        throw std::exception();
    //当自由空间被耗尽时，new表达式就会失败，并抛出一个类型为bad_alloc的异常，所以我觉得这一步判断是画蛇添足

    for(int i = 0; i < m_thread_number; ++i){
        if(pthread_create(&m_threads[i], NULL, worker, this) != 0){
        //int pthread_create(pthread_t* thread,const pthread_attr_t* attr, void*(*start_routine)(void*), void* arg);
            //第一个参数是 类型为指针的传出参数，线程创建好后，将线程id赋值给这个指针所指的地址
            //第二个参数 NULL表示使用默认属性
            //第三个参数 指定新线程将运行的函数，是一个返回值void*，参数void*的函数指针。
                //这个函数不能是类的非静态成员函数，因为每个非static成员函数第一个参数都是一个隐式的this指针
                //也就是说，形参类型是这样的一个函数指针 void* (*f1)(void*)，而成员函数名作函数指针是这样的：void* (*f2)(this*)
                //冷知识：两个不同的函数指针之间无法进行类型转换，例如：invalid conversion from ‘void (*)(int)’ to ‘void (*)(double)’
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

//将http请求放进请求队列。该函数适用于Reactor模式。参数state是该http连接处于的阶段是读阶段还是写阶段
template<typename T>
bool threadpool<T>::append(T* request, int state){

    m_queuelocker.lock(); //对请求队列上锁

    if(m_workqueue.size() >= m_max_requests){
        m_queuelocker.unlock();  //解锁
        return false;
    }

    request->m_state = state;  //设置http连接状态
    m_workqueue.push_back(request);
    m_queuelocker.unlock(); //解锁
    m_queuestat.post();  //对代表 请求队列中的请求数的信号量 进行V操作
    return true;

}

//Proactor模式的append。
//Proactor模式不需要state参数，是因为：Proactor模式下，工作线程不需要负责IO，也就无所谓读阶段还是写阶段
template<typename T>
bool threadpool<T>::append_p(T* request){ 
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

//工作线程从请求队列中取出某个任务进行处理
//如果是Reactor模式，需要工作线程负责IO。Proactor模式不需要工作线程负责IO，只处理业务逻辑即可
template<typename T>
void threadpool<T>::run(){    //run函数暂时没法写，因为涉及到具体业务逻辑，下面仅为测试用

    while(true){
        
        m_queuestat.wait();  //P操作，对代表 请求队列中请求数的信号量 进行P操作。

        m_queuelocker.lock(); //请求队列是共享资源，需要上锁

        //如果请求队列为空
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        /*信号量仅起到标志请求队列中是否有请求，当队列为空时，信号量为0，会阻塞，减少cpu消耗。
            由于wait与lock是两步操作，并非原子操作，wait成功后，还需要获取请求队列的锁，所以还需要等待。
            当获取锁之后，由于请求队列中 请求可能已经被消耗完，所以需要判断队列是否为空
        */
        /*信号量的值并不能准确的代表队列中的请求数。
            在append函数中，我们是先把请求放进队列，释放了锁，然后操post的信号量。
            在run函数中，我们是先wait的信号量，然后才加锁，尝试取出请求
            也就是说，在一些时刻，信号量的值并不是准确的，并不真正代表请求队列中的请求数。
            我们只能用信号量做一些粗略的判断，比如判断队列中是否有请求，没有就阻塞住
        */ 

        //从请求队列中取出第一个任务，然后从请求队列移除
        T *request = m_workqueue.front();
        m_workqueue.pop_front();

        //解锁请求队列
        m_queuelocker.unlock();

        //如果front取任务失败
        if (!request)
            continue;

        /*Reactor模式：主线程(I/O处理单元)只负责监听文件描述上是否有事件发生，然后通知工作线程(逻辑单元)。
        工作线程需要 处理业务逻辑 和I/O（数据的接收和发送）*/
        if (m_actor_model == 1)
        {
            //对该http请求的处理处于 读阶段：将请求报文读到缓冲区，并解析，然后将响应报文写入写缓冲区
            if (request->m_state == 0)
            {
                //工作线程负责IO：将接收到的数据放到读缓冲区
                int res = request->read_once();

                //标记该连接执行过IO操作
                request->improv = 1;
                if (res)
                {
                    //从连接池中取出一个数据库连接
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }

                //IO失败，设置标记timer_flag，标记该连接应该被关闭
                else
                {
                    request->timer_flag = 1;
                }
            }
            //对该http请求的处理处于 写阶段：将写缓冲区中的响应报文发送给浏览器端
            else
            {
                //工作线程负责IO：将相应报文放进写缓冲区
                int res = request->write();

                //标记该连接执行过IO操作
                request->improv = 1;

                //IO失败，设置标记timer_flag，标记该连接应该被关闭
                if (res == 0)
                {
                    request->timer_flag = 1;
                }
            }
        }

        //Proactor模式：所有I/O操作都交给主线程和内核来处理，也就是数据的接收和发送交给主线程
        //工作线程仅仅负责业务逻辑，也就是 解析 读缓冲区的请求报文，然后把响应报文写到 写缓冲区中
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }

    }
}

#endif