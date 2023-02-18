#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <sys/time.h> //几个时间结构体和gettimeofday函数的头文件
#include <stdlib.h> 
#include"../locker.h"

//为了生成一个实例化版本，编译器需要掌握类模板成员函数的定义。因此，模板的头文件通常既包括声明也包括定义
template<typename T>
class block_queue{
private:
    locker m_mutex;
    cond m_cond;

    T* m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;

public:
    block_queue(int max_size = 1000){
        if(max_size < 0)
            exit(-1);
        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = 0;
        m_back = 0;  //左闭右开区间的队列，front指向队首元素，back指向下一个空位
    }

    void clear(){
        m_mutex.lock(); //为了保证线程安全，我们对阻塞队列这个共享资源的每一个操作都是加锁然后解锁的
        m_size = 0;
        m_front = 0;
        m_back = 0;
        m_mutex.unlock();
    }

    ~block_queue(){
        m_mutex.lock();
        delete[] m_array;
        m_mutex.unlock();
    }

    bool full(){ //判断队列是否满
        m_mutex.lock();
        if(m_size >= m_max_size){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    bool empty(){ 
        m_mutex.lock();
        if(m_size == 0){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    bool front(T& value){ //使用传出参数返回
        m_mutex.lock();
        if(m_size == 0){
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front]; //将队首元素赋值给value。由于value是一个引用，所以函数外value的值也改变了
        m_mutex.unlock();
        return true;
    }

    bool back(T& value){
        m_mutex.lock();
        if(m_size == 0){
            m_mutex.unlock();
            return false;
        }
        value = m_array[(m_back - 1) % m_max_size]; 
        m_mutex.unlock();
        return true;
    }


    int size(){
        int tmp = 0; 
        //m_size是共享资源，访问它是需要加锁的
        //m_size是我们需要返回的资源，我们无法在一个函数中实现先返回m_size再解锁，所以采用了一个临时变量，
        //保存某一时刻m_size的值，然后解锁，返回这个临时变量
        m_mutex.lock();
        tmp = m_size;
        m_mutex.unlock();
        return tmp;
    }
    
    int max_size(){
        int tmp = 0;
        m_mutex.lock();
        tmp = m_max_size;
        m_mutex.unlock();
        return tmp;
    }

    bool push(const T& item){
        m_mutex.lock();

        if(m_size >= m_max_size){ //队列已满
            m_cond.broadcast(); //直接唤醒所有等待此条件变量的线程，然后返回
            m_mutex.unlock();
            return false;
        }
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        m_size++;

        m_cond.broadcast(); //唤醒所有等待此条件变量的线程
        m_mutex.unlock();
        return true;
        
    }

    bool pop(T& item){
        m_mutex.lock();
        while(m_size <= 0){ //pop时,如果当前队列没有元素,将会等待条件变量

            bool res = m_cond.wait(m_mutex.get());
            //要获取locker里面封装的的pthread_mutex_t需要用get接口
            //wait函数成功被唤醒会返回true，否则false

            if(res == false){
                m_mutex.unlock();
                return false;
            }

        }//使用pthread_cond_wait函数的流程其实就是 1.加锁 2.循环pthread_cond_wait 3.解锁，但是考虑到pthread_cond_wait可能会失败，出于程序的健壮性，加了个对返回值的判断

        item = m_array[m_front];
        m_front = (m_front + 1) % m_max_size;
        m_size--;

        m_mutex.unlock();
        return true;
    }

    bool pop(T& item, int ms_timeout){ //超时pop，单位毫秒，超过给定时间就失败返回

        //1s = 10^3毫秒 = 10^6微秒 = 10^9纳秒

        struct timespec t = {0, 0}; //要设置的超时时间（距1970年）。初始化为0秒，0纳秒，
        struct timeval now = {0, 0}; //现在的时间。初始化为0秒，0毫秒
        gettimeofday(&now, NULL); //将Unix纪元时间放进now结构体里
        m_mutex.lock();

        if(m_size <= 0){ //如果当前队列没有元素,将会等待条件变量
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000000; //1毫秒是10^6纳秒

            int res = m_cond.timedwait(m_mutex.get(), t);
            if(res == 0){ //在给定时间内该线程没有被唤醒，函数就返回，返回的原因可能是超时了，也可能是其他原因导致函数执行失败
                m_mutex.unlock();
                return false;
            }
            
        }

        if(m_size <= 0){ //线程被唤醒后，如果队列中依旧没有元素，也false
            m_mutex.unlock();
            return false;
        }

        item = m_array[m_front];
        m_front = (m_front + 1) % m_max_size;
        m_size--;

        m_mutex.unlock();
        return true;

    }

};



#endif