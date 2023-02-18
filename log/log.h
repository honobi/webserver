#ifndef LOG_H
#define LOG_H

#include <iostream>  //FILE头文件
#include <stdio.h>   //fputs、fopen等与文件操作有关的头文件
#include "block_queue.h"

class Log{
private:
    char dir_name[128]; //日志文件存放的路径
    int m_split_lines; //单个日志文件最大日志条数，这个并不是行数，因为一条日志并不止一行
    int m_file_cnt;    //当日创建的日志文件数
    int m_log_buf_size; //日志缓冲区大小
    char* m_buf;  //日志缓冲区，存放一条日志
    long long m_count; //当前日志总行数
    int m_today; //记录当天是哪一天，方便判断是否到了第二天，创建日志新文件
    FILE* m_fp;  //打开log的文件指针
    int m_close_log;  //是否关闭日志功能
    bool m_is_async; //是否开启异步

    block_queue<std::string>* m_log_queue; //阻塞队列
    locker m_mutex;
    

    Log(); //单例模式，构造函数设置为private
    ~Log(); 
    
    void* async_write_log(); //异步日志

public:
    static Log* get_instance(){
        static Log instance;
        return &instance;
    }

    static void* async_log_thread(void* args){ 
        //async_log_thread与async_write_log函数的关系与线程池里的worker和run的关系是一样的
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
#define LOG_DEBUG(format, ...) if(m_close_log == 0) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(m_close_log == 0) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(m_close_log == 0) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(m_close_log == 0) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}
//...表示可变参数，__VA_ARGS__就是将...对应的实参替换到到该处。
//##的作用：在某次宏调用时，如果##后的形参对应的实参个数为0个，那么把形参替换为空


#endif