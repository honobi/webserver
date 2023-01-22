#ifndef LOG_H
#define LOG_H

#include <iostream>  //FILE头文件
#include <stdio.h>   //fputs、fopen等与文件操作有关的头文件
#include "block_queue.h"

class Log{
private:
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines; //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count; //当前日志行数
    int m_today; //记录当天是哪一天，方便判断是否到了第二天，创建日志新文件
    FILE* m_fp;  //打开log的文件指针
    char* m_buf;  //要输出的内容
    block_queue<std::string>* m_log_queue; //阻塞队列
    bool m_is_async; //是否同步的标志位
    locker m_mutex;
    int m_close_log;  //关闭日志功能

    Log(); //构造函数设置为private
    virtual ~Log(); //类里面一个虚函数都没有，不知道为什么要把析构设置为vitual的，难道未雨绸缪？可是vitual是需要额外成本的
    
    void* async_write_log(){ //异步写日志
        std::string single_log;
        while(m_log_queue->pop(single_log)){ //从阻塞队列中取出一条日志
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);//写入文件
            /*int fputs ( const char * str, FILE * stream ) 
            将C字符串写入流，一直读到\0为止，但不会把\0写入流
            */
            m_mutex.unlock();
        }
    }

public:
    static Log* get_instance(){
        static Log instance;
        return &instance;
    }

    static void* flush_log_thread(void* args){ 
        //这个函数的作用与线程池里的run和worker关系是一样的，都是因为创建线程不能使用非static成员函数
        get_instance()->async_write_log(); 
    }

    //初始化时设置一系列参数
    bool init(const char* file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);
    
    //将输出内容按照格式写入文件
    void write_log(int level, const char* format, ...);//省略符形参。可变参数：表示除了level和format是固定的，后面跟的参数的个数和类型是可变的

    //强制刷新缓冲区
    void flush();
 

};

//这四个宏定义在其他文件中使用，主要用于不同类型的日志输出
#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}
//...表示可变参数，__VA_ARGS__就是将...对应的实参替换到到该处。
//##的作用：在某次宏调用时，如果##后的形参对应的实参个数为0个，那么把形参替换为空


#endif