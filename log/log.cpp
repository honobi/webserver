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

//初始化配置参数，确定异步还是同步，创建日志缓冲区，打开日志文件
bool Log::init(const char* dir, int close_log, int log_buf_size, int split_lines, int max_queue_size){
    //如果是异步，则需要设置阻塞队列的长度，同步不需要设置，max_queue_size的默认参数是0

    strcpy(dir_name, dir);
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_split_lines = split_lines;
    m_file_cnt = 0;
    //创建并清空日志缓冲区
    m_buf = new char[m_log_buf_size]; 
    memset(m_buf, 0, m_log_buf_size); 


    //异步日志
    if(max_queue_size > 0){ //阻塞队列长度大于0，那么异步写入
        m_is_async = true;

        //创建阻塞队列
        m_log_queue = new block_queue<string>(max_queue_size); 

        //创建新线程，让该线程异步写日志
        pthread_t tid; 
        pthread_create(&tid, NULL, async_log_thread, NULL); 
    }

    //下面的全部操作都是：确定文件名，然后创建日志文件

    //读取file_num文件中 当日日志文件数
    char* file_num_name = new char[256];
    strcpy(file_num_name, dir_name);
    strcat(file_num_name, "/file_num");
    FILE* file_num = fopen(file_num_name, "a+");
    int read_res = fread(&m_file_cnt, sizeof(int), 1, file_num);
    //如果文件为空，写入当日日志文件数：0
    if(read_res == 0){
        fwrite(&m_file_cnt, sizeof(int), 1, file_num);
    }
    
    
    //获取当前时间
    time_t t = time(NULL); //返回Unix纪元至今的秒数
    struct tm* sys_tm = localtime(&t); //将秒数时间转换为日历形式的本地时间tm
    struct tm my_tm = *sys_tm;  
    //localtime()函数返回的是一个静态变量的指针，下一次再调用localtime内容就变了，所以我们需要用一个局部变量把时间保存下来

    m_today = my_tm.tm_mday;

    char log_full_name[256] = {0}; //日志文件名

    snprintf(log_full_name, 255, "%s//%d_%02d_%02d_Log_%d", dir_name,  my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, m_file_cnt + 1);

    m_fp = fopen(log_full_name, "a+"); //创建新文件

    //更新文件数，并写入文件
    m_file_cnt++;
    fwrite(&m_file_cnt, sizeof(int), 1, file_num);
    fclose(file_num);

    if(m_fp == NULL){
        return false; //文件打开/创建失败，那么init失败
    }
    return true;

}

//先向缓冲区写入一条日志，然后再把缓冲区的字符串写到文件(同步或异步)
void Log::write_log(int level, const char* format, ...){
    /*日志中的时间需要用日历类型来记录，而且精度需要精确到微秒，但是日历类型时间最多只精确到秒，
      方法：先用一个gettimeofday获取 秒+微秒，然后把秒转换为日历类型，再加上微秒，就得到了精确到微秒的日历类型*/
    struct timeval now = {0, 0}; //{秒，微秒}
    gettimeofday(&now, NULL); //将Unix纪元时间放进now中
    time_t t = now.tv_sec; 
    struct tm* sys_tm = localtime(&t); //秒数类型转换为本地日历类型
    struct tm my_tm = *sys_tm;
    //localtime函数返回的是一个静态变量的指针，需要用一个局部变量把时间保存下来

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

    //如果时间到了第二天，或者日志到达了最大行数，需要创建新的日志文件
    if(m_today != my_tm.tm_mday || m_count % m_split_lines == 0){ 

        char new_log[256] = {0};
        fclose(m_fp); //刷新文件缓冲区 并 关闭文件描述符

        char day[16] = {0}; //保存日期
        snprintf(day, 16, "%d_%02d_%02d", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        //如果是时间到了第二天
        if(m_today != my_tm.tm_mday){ 
            m_file_cnt = 0;
            snprintf(new_log, 255, "%s//%s_Log_%d", dir_name, day, m_file_cnt + 1);
            m_today = my_tm.tm_mday; 
            m_count = 0; 

        }
        //如果是日志到达了单个日志最大行数，那么新文件的名字是原文件名+1,2,3,4....表示当天的第几个文件
        else{ 
            snprintf(new_log, 255, "%s//%s_Log_%d", dir_name, day, m_file_cnt + 1);
        }
        //创建新文件
        m_fp = fopen(new_log, "a+"); 
        //更新当日文件数，并写入文件
        m_file_cnt++;
        char* file_num_name = new char[256];
        strcpy(file_num_name, dir_name);
        strcat(file_num_name, "/file_num");
        FILE* file_num = fopen(file_num_name, "a+");
        fwrite(&m_file_cnt, sizeof(int), 1, file_num);
        fclose(file_num);
    
    }
    m_mutex.unlock();

    va_list valst; //指向可变参数的指针
    va_start(valst, format); //va_start 宏初始化刚定义的va_list变量

    m_mutex.lock();

    //向缓冲区的开头写入时间，n为写入的字符数
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);

    //向缓冲区写入日志内容，m为写入的字符数
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    //vsnprintf接受可变参数va_list
    
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';   //此时\0之后可能有之前的数据，但是都在\0后面，就不会影响当前要写入的字符串


    m_mutex.unlock();

    //如果是异步日志，而且阻塞队列没满，那么把要写入的日志放进阻塞队列
    if(m_is_async && m_log_queue->full() == false){  
        string log_str;
        m_log_queue->push(log_str); 
    }
    //如果是同步日志，那么直接往文件里写
    else{ 
        m_mutex.lock();
        fputs(m_buf, m_fp); //将C字符串写入流，一直读到\0为止，但不会把\0写入流
        m_mutex.unlock();
    }

    va_end(valst); //va_end宏，清空va_list可变参数列表，结束可变参数的获取

}

void* Log::async_write_log(){ 
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

void Log::flush(){ 
    m_mutex.lock();
    fflush(m_fp); //刷新文件缓冲区
    m_mutex.unlock();
}

