#ifndef LOCKER_H
#define LOCKER_H

#include<semaphore.h>
#include<exception> //该头文件定义了异常类exception
#include<pthread.h> //linux多线程编程的接口。互斥锁和条件变量的头文件，因此他们俩的名字和函数其实都是pthread_开头的

//信号量
class sem{ 
public:
    sem(){ //默认构造，信号量的初值设定为0
        if(sem_init(&m_sem, 0, 0) != 0){ //linux的函数大多是成功返回0，失败返回错误码
            //初始化m_sem信号量，第二个0指定信号量类型为局部信号量，第三个0指定初值为0
            throw std::exception(); //异常类exception只报告异常的发生，不提供任何信息
        }
    }
   
    sem(int num){ //单参构造，信号量初值设定为num
        if(sem_init(&m_sem, 0, num) != 0)
            throw std::exception(); //构造一个匿名的 exception对象 并直接抛出
    }
    
    ~sem(){
        sem_destroy(&m_sem); //销毁信号量，以释放其占用的内核资源
    }

    bool wait(){ //P操作
        return sem_wait(&m_sem) == 0; 
    }

    bool post(){ //V操作
        return sem_post(&m_sem) != 0;
    }

private:
    sem_t m_sem; //POSIX信号量
};

//互斥锁，互斥锁就相当于一个二进制的信号量
class locker{ 
public:
    locker(){
        if(pthread_mutex_init(&m_mutex, NULL) != 0) //第二个参数null表示使用默认属性。
            throw std::exception();
    }

    ~locker(){
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock(){
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock(){
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t* get(){
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex; //POSIX互斥锁
};

//条件变量，条件变量提供了一种线程间的通知机制
class cond{
public:
    cond(){

        if(pthread_cond_init(&m_cond, NULL) != 0){
            //第二个参数设置为NULL表示使用默认属性
            throw std::exception();
        } 
            
    }

    ~cond(){
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t* m_mutex){ //等待目标变量

        return pthread_cond_wait(&m_cond, m_mutex) == 0;
        //wait函数做了3件事：1.把调用线程放入条件变量的等待队列中 2.将互斥锁m_mutex解锁，随后进入等待状态
        //3.当其他线程通过signal或broadcast唤醒该线程时，wait函数返回，同时加锁m_mutex
        
    }

    bool timedwait(pthread_mutex_t* m_mutex, struct timespec t){ //具有等待时间的wait
        int ret = pthread_cond_timedwait(&m_cond, m_mutex, &t); //这个时间是从1970年到现在的时间间隔
    }

    bool signal(){ //唤醒等待条件变量的线程
        return pthread_cond_signal(&m_cond) == 0;
        /*发送一个信号给另外一个正在处于阻塞等待状态的线程,使其脱离阻塞状态,继续执行
        如果没有线程处在阻塞等待状态,pthread_cond_signal也会成功返回
        signal一般不会有“惊群现象”产生，他最多只给一个线程发信号。
        至于哪个线程将被唤醒，则取决于线程的优先级和调度策略
        */
    }

    bool broadcast(){ //以广播的方式唤醒所有等待目标条件变量的线程
        return pthread_cond_broadcast(&m_cond) == 0;
        /*采用广播方式唤醒 所有 等待条件变量 的线程，(这个所有真的是所有)
        这些线程被唤醒后都检查该变量以判断被唤醒的是否是自己，
        如果是就 开始执行后续代码，如果不是则返回继续等待
        */
    }

private:
    pthread_cond_t m_cond; 
    //条件变量要与互斥锁一起使用，因为条件变量是共享资源，所以signal和wait操作都需要上锁执行。
    //而至于wait函数内部为什么要解锁再上锁，原因是通过互斥锁实现 线程同步
    //可以看https://blog.csdn.net/itworld123/article/details/115654491 的图
    /*这个图画的很好，展示了使用信号量应该遵循的步骤
        第一步是wait线程上锁mutex。可以看到signal线程第一步要做的也是上锁mutex。
        所以我们只要保证wait线程先上锁，然后signal线程再上锁，就可以保证，pthread_cond_signal函数一定是在pthread_cond_wait函数后面运行
        我们需要自己手动上锁再解锁的就两个地方：wait线程：1.上锁 2.调用pthread_cond_wait 3.解锁
        signal线程：1.上锁 2.调用pthread_cond_signal 3.解锁
        而pthread_cond_wait函数中还会自动执行一次 ：1.解锁 2.阻塞等待被唤醒 3.唤醒后上锁
        所以总的执行流程是这样的：
        1.wait线程上锁  2.wait线程调用pthread_cond_wait函数，这个函数会自动解锁
        3.signal线程上锁    4.signal线程调用pthread_cond_signal唤醒wait线程
        5.signal线程解锁  6.wait线程的pthread_cond_wait函数自动上锁    7.wait线程解锁
    */

    //在实践中，我们必须确保wait线程先上锁，然后signal线程在上锁，必须保证这一点，才能做到线程同步
    //否则signal调用时，wait还没开始，然后啥也没唤醒，然后开始wait，就不会再被唤醒了，一直阻塞着。
};

#endif