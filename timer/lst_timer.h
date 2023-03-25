#ifndef LST_TIMER
#define LST_TIMER

#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h> //memset头文件
#include <error.h>
#include<sys/types.h>
#include <sys/socket.h> 
#include <assert.h>
#include <unistd.h>
#include <vector>
#include <unordered_map>
#include <arpa/inet.h>
#include "../log/log.h"
#include "../http/http_conn.h"

class util_timer; //前向声明

//连接资源的结构体
struct client_data{
    sockaddr_in address; //客户端socket地址
    int sockfd;         //socket文件描述符
    util_timer* timer; //该连接资源使用的定时器
};

//定时器类
class util_timer {   
    //util 是 utility 的缩写，表示实用工具、工具类
public:
    time_t expire;  //到期时间
    void (*cb_func)(client_data*); //当定时器超时时，用于处理该定时器的回调函数
    client_data* user_data;  //连接资源

    util_timer(){}
};

//定时器最小堆
class timer_heap{
public:

    void add_timer(util_timer *timer); //添加定时器
    void adjust_timer(util_timer *timer); //调整定时器位置
    void del_timer(util_timer *timer); //删除定时器
    void tick(); //处理所有的超时定时器

    void m_swap(int i, int j);
    void siftup(int i);
    void siftdown(int i);

private:
    std::vector<util_timer*> min_heap;
    std::unordered_map<util_timer*, int> umap;
};


//工具类
class Utils{
public:
    static int* u_pipefd; //管道写端
    static int u_epollfd; //epoll文件描述符

    timer_heap m_timer_heap; //定时器容器类
    int m_TIMESLOT; //超时时间

public:
    Utils() {}
    ~Utils() {}
    
    void init(int timeslot, int epollfd, int* pipefd);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    void add_read_event(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    //向服务端发送错误信息。用于当连接数超出上限后，向客户端发送错误信息
    void show_error(int connfd, const char *info);

};

//处理超时连接的回调函数
void cb_func(client_data *user_data);


#endif