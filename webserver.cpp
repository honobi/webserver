#include "webserver.h"

using namespace std;

WebServer::WebServer() {

    //在程序一开始就创建MAX_FD个http_conn对象 和 包含定时器的连接资源结构体
    users = new http_conn[MAX_FD];
    users_timer = new client_data[MAX_FD];

    //确定root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);  //获取当前工作目录的绝对路径，并放到server_path中
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);
    
}

WebServer::~WebServer() {
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(Config* config) {
    m_port = config->PORT;
    m_user = config->user;
    m_passWord = config->passwd;
    m_databaseName = config->databasename;
    m_sql_num = config->sql_num;
    m_thread_num = config->thread_num;
    m_log_write = config->LOGWrite;
    m_OPT_LINGER = config->OPT_LINGER;
    m_TRIGMode = config->TRIGMode;
    m_close_log = config->close_log;
    m_actormodel = config->actor_model;
}

//设置listenfd和http连接上读写事件的触发模式
void WebServer::trig_mode() {
    //LT + LT
    if (m_TRIGMode == 0)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (m_TRIGMode == 1)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (m_TRIGMode == 2)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (m_TRIGMode == 3)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write() {

    //不关闭日志，也就是打开日志功能
    if (m_close_log == 0) {

        //异步日志，阻塞队列大小设置为800
        if (m_log_write == 1)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        //同步日志
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool() {
    //初始化数据库连接池
    m_connPool = connection_pool::get_instance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //从数据库user表中取出用户名和密码放到map容器中
    users->initmysql_result(m_connPool);
}

//线程池
void WebServer::thread_pool() {
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}


void WebServer::event_listen() {

    //创建socket
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //使用SO_LINGER选项
    if (m_OPT_LINGER == 1) {
        struct linger tmp = {0};
        tmp.l_onoff = 1;
        tmp.l_linger = 1;
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
        /*默认情况下，无论是阻塞socket还是非阻塞socket。当我们使用close系统调用来关闭一个socket时，
        close 将立即返回，TCP模块负责把该socket对应的TCP发送缓冲区中残留的数据发送给对方。*/
        //linger结构体的l_onoff(开关)成员设置为1，表示该选项开启
        /*如果socket是阻塞的，close 将等待一段长为l_linger的时间，让之前已发送但尚未确认的数据被对端接收到并发送ACK确认，
            如果在这个时间内数据传输完毕，close函数将返回0；否则，将返回-1，并设置errno为EWOULDBLOCK（或者EAGAIN）
          如果socket是非阻塞的，close函数会立即返回，并且不会等待之前已发送但尚未确认的数据被对端接收到。
            这时候，如果剩余数据没有发送完毕，这些数据将会被丢弃，也就是说它们永远不会被发送给对端 */
        
    }

    //创建socket地址，并设置协议族、端口号、IP
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(m_port);
    address.sin_addr.s_addr = INADDR_ANY;  //INADDR_ANY为0（0.0.0.0），表示本机所有ip
    //或者：inet_pton(AF_INET, "0.0.0.0", &address.sin_addr); // 将点分十进制IP地址转换为网络字节序的二进制IP地址
    
    //端口复用
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    int ret = 0;

    //将监听socket与socket地址绑定
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    //监听socket上的连接
    ret = listen(m_listenfd, 5); //将TCP的SYN队列的最大长度设置为5
    assert(ret >= 0);

    //初始化工具类
    utils.init(TIMESLOT, m_epollfd, m_pipefd);  

    //epoll创建内核事件表
    m_epollfd = epoll_create(114514);
    assert(m_epollfd != -1);

    //向事件表注册一个事件，去检测listenfd上是否有新连接
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;   //整个程序使用同一个epoll实例

    //使用socketpair创建一个全双工管道
    //实际是建立一对匿名的已经连接的socket，把创建好的socket的文件描述符放进m_pipefd数组
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);

    //将管道写端设置为非阻塞
    utils.setnonblocking(m_pipefd[1]);
    /*为什么要讲管道的读端设置为非阻塞：信号处理函数使用send函数将信息写到管道写端，如果写端满了，
        则会阻塞，这时候会进一步增加信号处理函数的执行时间
    设置为非阻塞，而且没有对返回值进行处理，如果缓冲区满，就意味着这一次信号处理失败了
        但本程序的信号主要是SIGALRM（定时关闭连接），并不是必须立即处理的事件，可以允许这样的情况发生。
    */

    //检测管道读端的可读事件
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

}

void WebServer::set_sigact(){

    //忽略SIGPIPE信号
    utils.addsig(SIGPIPE, SIG_IGN); 
    
    //捕捉SIGALRM和SIGTERM信号，处理行为：将信号值写入管道写端
    utils.addsig(SIGALRM, utils.sig_handler, false);  //SIGALRM信号是alarm函数定时结束时发出的信号
    utils.addsig(SIGTERM, utils.sig_handler, false);  //SIGTERM信号是kill命令不加任何选项时，默认发送15号命令，也就是SIGTERM

    //TIMESLOT之后发送alarm信号
    alarm(TIMESLOT);

}


//初始化http连接并使用定时器管理该连接
void WebServer::timer(int connfd, struct sockaddr_in client_address) {
    //初始化http连接
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //创建定时器
    util_timer* timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;  //超时时间设置为3个TIMESLOT

    //初始化连接资源
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    users_timer[connfd].timer = timer;

    //将定时器放进最小堆
    utils.m_timer_heap.add_timer(timer);
}

//刷新定时器：当前时间+3个TIMESLOT
void WebServer::flush_timer(util_timer *timer) {
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_heap.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

//关闭http连接
void WebServer::close_conn(util_timer *timer, int sockfd) {

    //调用回调关闭超时连接：关闭socket文件描述符和去除epoll对它的事件检测
    timer->cb_func(&users_timer[sockfd]);

    //将该定时器从最小堆移除
    if (timer) 
        utils.m_timer_heap.del_timer(timer);

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

//处理客户端数据：使用accept函数接收客户端连接，并调用timer()初始化一个http连接并使用定时器管理
bool WebServer::dealclinetdata() {
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    //如果对listenfd的检测的 触发模式是LT模式，那么如果一个事件没有被处理，之后调用epoll_wait还会触发该事件
    if (m_LISTENTrigmode == 0) {

        //接收客户端连接：由系统（内核）创建一个socket并由accept函数返回他的文件描述符
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        
        //accept失败
        if (connfd < 0) {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }

        //当前连接数到达上限
        if (http_conn::m_user_count >= MAX_FD) {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }

        //初始化http连接并使用定时器管理
        timer(connfd, client_address);
    }

    //如果对listenfd的检测是ET触发模式，则需要循环的调用accept去接受连接，因为ET模式每个事件只触发一次，如果有未被处理的连接，之后调用epoll_wait也不会触发
    else {
        while (1) {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0) {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD) {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

//判断接收到的是SIGALRM还是SIGTERM信号，并使用传出参数传出去
bool WebServer::dealwithsignal(bool& timeout, bool& stop_server) {

    int ret = 0;
    int sig;
    char signals[1024];

    //从管道读端 获取信号值，放进signals
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);

    //失败
    if (ret == -1)
        return false;
    
    //没有数据
    else if (ret == 0)
        return false;
    
    //管道读端有数据，且字节数为ret，则信号数为ret个
    else {
        for (int i = 0; i < ret; ++i) {
            switch (signals[i]) {
                case SIGALRM: {   //alarm函数倒计时为0时会触发该信号
                    timeout = true;
                    break;
                }
                case SIGTERM: { //kill命令会发送该信号
                    stop_server = true;
                    break;
                }
            }
        }
    }
    return true;
}

//socket上有可读事件发生时，调用该函数处理事件
//Reactor模式与Proactor模式下，主线程有不同操作
//Reactor模式，主线程只通知工作线程，然后工作线程负责IO和业务处理
//Proactor模式（同步IO模拟）：主线程负责IO，工作线程负责业务处理
//如果IO操作失败，那么应该立即关闭该连接以节省资源
void WebServer::dealwithread(int sockfd) {
    //timer为该连接资源的定时器
    util_timer* timer = users_timer[sockfd].timer;

    //Reactor模式，主线程只负责监听文件描述符上是否有事件发生，然后通知工作线程。工作线程负责IO和业务处理
    if (m_actormodel == 1) {

        //刷新定时器
        if (timer)
            flush_timer(timer);


        //将该http连接放入请求队列，也就是通知工作线程。设置http状态为：读阶段
        m_pool->append(&users[sockfd], 0);

        while (true) {
            //如果在工作线程中执行过IO操作
            if (users[sockfd].have_io == 1) {

                //如果该连接的IO操作出错，就该关闭连接
                if (users[sockfd].shoule_close == 1) {
                    //关闭连接
                    close_conn(timer, sockfd);
                    users[sockfd].shoule_close = 0;   //重置timer_flag，
                }
                users[sockfd].have_io = 0;   //重置improv
                break;
            }
        }
    }
    else {
        /*Proactor模式（同步IO模拟）：主线程执行数据读写操作，读写完成之后，主线程向工作线程通知这一“完成事件”。
        那么从工作线程的角度来看，它们就直接获得了数据读写的结果，接下来要做的只是对读写的结果进行逻辑处理。*/
        
        //主线程 读取数据
        bool ret = users[sockfd].read_once();

        //读数据成功
        if (ret) {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //将该http连接放入请求队列
            m_pool->append_p(&users[sockfd]);

            //刷新定时器
            if (timer)
                flush_timer(timer);
            
        }

        //IO操作失败，应该关闭该连接
        else {
            close_conn(timer, sockfd);
        }
    }
}

//socket上有可写事件发生时，调用该函数处理事件
//Reactor模式与Proactor模式下，主线程有不同操作
//Reactor模式，主线程只通知工作线程，然后工作线程负责IO和业务处理
//Proactor模式（同步IO模拟）：主线程负责IO，工作线程负责业务处理
//如果IO操作失败，那么应该立即关闭该连接以节省资源
//该函数与dealwithread几乎一致，只是读变成了写
void WebServer::dealwithwrite(int sockfd) {
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (m_actormodel == 1) {
        if (timer)
            flush_timer(timer);

        m_pool->append(&users[sockfd], 1);

        while (true) {
            if (users[sockfd].have_io == 1) {
                if (users[sockfd].shoule_close == 1) {
                    close_conn(timer, sockfd);
                    users[sockfd].shoule_close = 0;
                }
                users[sockfd].have_io = 0;
                break;
            }
        }
    }
    //proactor
    else {
        
        if (users[sockfd].write()) {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //重置定时器
            if (timer)
                flush_timer(timer);

            //如果是短连接，那么关闭连接
            if(users[sockfd].if_keep_alive()== false)
                close_conn(timer, sockfd);
            
        }
        //write失败，关闭连接
        else {
            close_conn(timer, sockfd);
        }
    }
}


void WebServer::event_loop() {

    bool timeout = false;       //是否接收到SIGALRM信号（alarm函数设定的定时器倒计时结束时发出该信号）
    bool stop_server = false;   //是否接收到SIGTERM信号（kill命令默认发出的信号）

    while (!stop_server) {
        //epoll_wait等待事件表中的某个事件发生。超时时间-1表示该函数永久阻塞，直到某个事件发生
        int num = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        //成功时返回就绪的文件描述符的个数，失败时返回-1并设置errno。

        if (num < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < num; i++) {
            int sockfd = events[i].data.fd;

            //有新的连接
            if (sockfd == m_listenfd) {

                //处理新连接
                bool flag = dealclinetdata();
                if (flag == false)
                    continue;
            }
            //对端断开TCP连接或 文件描述符发生错误
            else if ( (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0){  //位运算优先级很低，比==和!=都低，但是比&&和||要高

                //关闭连接
                util_timer* timer = users_timer[sockfd].timer;
                close_conn(timer, sockfd);
            }
            //如果管道写端有信号需要处理。目前只处理SIGALRM和SIGTERM信号
            else if ( ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) != 0) {

                //判断管道写端是什么信号。timeout和stop_server是传入传出参数，
                bool flag = dealwithsignal(timeout, stop_server);
                if (flag == false)
                    LOG_ERROR("%s", "dealwithsignal failure");
            }
            //除上面以外，如果有可读事件，表示TCP连接接收到新数据
            else if ( (events[i].events & EPOLLIN) != 0 )
                dealwithread(sockfd);
            
            //如果有可写事件，表示需要发送数据
            else if ( (events[i].events & EPOLLOUT) != 0)
                dealwithwrite(sockfd);
        }

        //如果接收到SIGALRM信号，则处理所有超时连接，并重新启动 本进程 的定时器以 定时处理超时连接
        //区别两个定时器：util_timer定时器是我们自己定义的定时器类，而alarm函数启动的定时器是每个进程仅有一个的定时器
        if (timeout) {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }

}

