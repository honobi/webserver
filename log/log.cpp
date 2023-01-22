#include <pthread.h>
#include <string.h> //memset头文件
#include <time.h>
#include <stdarg.h> //va_list和va_start、va_end头文件
#include <stdio.h> //fputs、fopen、fclose、fflush等与文件操作有关的头文件，还是snprintf、vsnprintf的头文件
#include "log.h"


using namespace std;
Log::Log():m_count(0), m_is_async(false){ 
    //日志行数初始化为0，以同步的方式启动
    
}

Log::~Log(){
    if(m_fp != NULL)
        fclose(m_fp); //关闭文件描述符
}

bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size){
    //如果是异步，则需要设置阻塞队列的长度，同步不需要设置，max_queue_size的默认参数是0

    if(max_queue_size > 0){ //当设置了阻塞队列长度，且大于0，那么异步写入
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size); //创建阻塞队列
        pthread_t tid; //线程id，作为创建线程的传出参数
        pthread_create(&tid, NULL, flush_log_thread, NULL); //创建线程，让他去异步写日志
    }

    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size]; //创建缓冲区
    memset(m_buf, 0, m_log_buf_size); //清空缓冲区
    m_split_lines = split_lines;

    time_t t = time(NULL); //返回Unix纪元至今的秒数
    struct tm* sys_tm = localtime(&t); //将秒数时间转换为日历形式的本地时间tm
    struct tm my_tm = *sys_tm;  
    //localtime()函数返回的是一个静态变量的指针，下次再调用localtime就变了，所以我们需要用一个局部变量把时间保存下来

    const char* p = strrchr(file_name, '/'); //在以\0结尾的字符串中寻找 该字符最后出现的位置，如果没出现，则返回值为NULL，否则返回字符位置的指针
    char log_full_name[256] = {0}; //保存将要创建的日志文件的名称

    //获取文件名称
    if(p == NULL){ //如果函数实参file_name不包含斜杠/，说明file_name是一个相对路径，那么直接以日期+file_name作为日志的文件名
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
        /*snprintf函数：int snprintf( char* buffer, std::size_t buf_size, const char* format, ... );
        将结果结果到字符串 buffer，至多写 buf_size - 1 个字符，产生的字符串会以空字符终止。
        %02d表示不足2位的使用前导0
        */
    }
    else{ //如果函数实参file_name包含斜杠/，说明file_name是绝对路径，我们需要把路径名和文件名分开来，然后以日期+不含路径的文件名 作为日志的文件名
        strcpy(log_name, p + 1); //将/之后的文件名存放到成员变量log_name中
        strncpy(dir_name, file_name, p - file_name + 1); //将file_name中的路径 保存到 成员变量dir_name中
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a"); //"a"以追加的方式打开文件，如果不存在则创建新文件

    if(m_fp == NULL){
        return false; //文件打开/创建失败，那么init失败
    }
    return true;

}

void Log::write_log(int level, const char* format, ...){
    struct timeval now = {0, 0}; //{秒，微秒}
    gettimeofday(&now, NULL); //将Unix纪元时间放进now中
    time_t t = now.tv_sec; //不知道为什么这三行多此一举，直接一个time函数不就代替这三行了吗
    struct tm* sys_tm = localtime(&t); //将秒数类型时间转换为本地日历类型时间
    struct tm my_tm = *sys_tm;
    //localtime()函数返回的是一个静态变量的指针，下次再调用localtime就变了，所以我们需要用一个局部变量把时间保存下来

    char s[16] = {0};
    switch(level){
        case 0 : 
            strcpy(s, "[debug]:");
            break;
        case 1:
            strcpy(s, "[info]:");
            break;
        case 2:
            strcpy(s, "[warn]:");
            break;
        case 3:
            strcpy(s, "[erro]:");
            break;
        default:
            strcpy(s, "[info]:");
            break;
    }

    m_mutex.lock(); //之后要访问共享资源，在这里上锁
    m_count++; //写入一条日志，那么日志行数+1

    if(m_today != my_tm.tm_mday || m_count % m_split_lines == 0){ //如果时间到了第二天，或者日志到达了最大行数

        char new_log[256] = {0};
        fflush(m_fp); //int fflush( FILE *stream );刷新缓冲区，将输出流stream的缓冲区的内容刷新到对应的输出设备中，在这里就是刷新到文件中
        fclose(m_fp);

        char tail[16] = {0}; //保存日期
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        if(m_today != my_tm.tm_mday){ //如果是时间到了第二天
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday; //更新当前是哪一天的成员变量
            m_count = 0; //日志行数清0
        }
        else{ //如果是日志到达了最大行数，那么新文件的名字是原文件名+1,2,3,4....表示当天的第几个文件
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }

        m_fp = fopen(new_log, "a"); //创建新文件

    }
    m_mutex.unlock();

    va_list valst; //指向可变参数的指针
    va_start(valst, format); //va_start 宏初始化刚定义的va_list变量

    string log_str; //提前构造log_str是为了不在加锁之后再构造，减少占用共享资源的时间
    m_mutex.lock();

    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    //n为写入的字符数
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst); //该函数第4个参数类型就是va_list，所以可以直接把valst传给他
    /*int vsnprintf( char* buffer, std::size_t buf_size, const char* format, va_list vlist );
        写结果到字符串 buffer 。至多写入 buf_size - 1 个字符。结果字符串将以空字符终止。
        返回值：若成功则返回写入的字符数 */
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if(m_is_async && m_log_queue->full() == false){  //如果是异步日志，而且阻塞队列没满
        m_log_queue->push(log_str); //把要写入的日志放进阻塞队列
    }
    else{ //如果是同步日志，或者阻塞队列满了，那么直接往文件里写
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp); //将C字符串写入流，一直读到\0为止，但不会把\0写入流
        m_mutex.unlock();
    }

    va_end(valst); //va_end宏，清空va_list可变参数列表，结束可变参数的获取

}

void Log::flush(){ //强制刷新缓冲区
    m_mutex.lock();
    fflush(m_fp); //刷新缓冲区，将输出流的缓冲区的内容刷新到对应的输出设备中，在这里就是刷新到文件中
    m_mutex.unlock();
}

