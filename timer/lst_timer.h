#ifndef LST_TIMER
#define LST_TIMER

#include <list>
#include <arpa/inet.h>
#include "../log/log.h"

class util_timer; //前向声明

//连接资源的结构体
struct client_data{
    sockaddr_in address; //客户端socket地址
    int sockfd;     //socket文件描述符
    util_timer* timer; //定时器，这里只需要一个util_timer的指针，所以只需要这之前有util_timer的声明即可，不需要完整定义
};

//定时器类
class util_timer{
public:
    time_t expire;  //该定时器被设定的目标时间
    void (*cb_func)(client_data*); //当定时器超时时，用于处理该定时器的回调函数
    client_data* user_data;  //连接资源

    util_timer(){}
};

//有序的定时器链表，之后可以考虑改成优先队列priority_queue会不会更好
class sort_timer_lst{
public:
    sort_timer_lst() {}
    ~sort_timer_lst();

    void add_timer(util_timer *timer); //添加定时器
    void adjust_timer(util_timer *timer); //调整定时器，任务发生变化时，调整定时器在链表中的位置
    void del_timer(util_timer *timer); //删除定时器
    void tick(); //处理所有的超时定时器

private:
    std::list<util_timer*> timer_list;
};


//该类实现的功能：定时处理非活动连接
class Utils{
public:
    static int* u_pipefd; //管道写端
    static int u_epollfd; //epoll文件描述符

    sort_timer_lst m_timer_lst; //定时器容器类
    int m_TIMESLOT; //超时时间

public:
    Utils() {}
    ~Utils() {}
    
    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

     //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

     //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);


};

//处理超时连接的回调函数
void cb_func(client_data *user_data);


#endif