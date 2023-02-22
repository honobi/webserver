#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "config.h"
#include "threadpool.h"
#include "http/http_conn.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

class Config; //前向声明

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(Config* config);

    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void event_listen();
    void set_sigact();
    void main_loop();
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd);
    bool dealclinetdata();
    bool dealwithsignal(bool& timeout, bool& stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

public:
    //基础
    int m_port;         //webserver放在哪个端口上
    char* m_root;       //root目录的绝对路径
    int m_log_write;    //0代表同步日志，1代表异步日志
    int m_close_log;    //日志是否关闭。0为否，1为是
    int m_actormodel;   //事件处理模式，1代表Reactor模式，2代表Proactor模式

    int m_pipefd[2];    //使用socketpair创建的管道的fd。0是读端，1是写端（实际是全双工的，但是我们只用到半双工）
    int m_epollfd;      //整个程序使用同一个epoll实例
    http_conn* users;   //http连接数组

    //数据库相关
    connection_pool *m_connPool;
    std::string m_user;         //登陆数据库使用的用户名
    std::string m_passWord;     //登陆数据库使用的密码
    std::string m_databaseName; //使用的数据库名
    int m_sql_num;

    //线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;   //线程池中的工作线程数量

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;
    int m_OPT_LINGER;  //监听socket是否使用socket的SO_LINGER选项
    int m_TRIGMode;     //listenfd和http连接上读写事件的触发模式(LT、ET)，共有2*2=4个组合
    int m_LISTENTrigmode;   //listenfd上事件的触发模式。1代表ET模式，0是LT模式
    int m_CONNTrigmode;     //http连接上读写事件的触发模式

    //定时器相关
    client_data *users_timer; //连接资源数组
    Utils utils;  //定时处理非活跃连接 的类
};
#endif
