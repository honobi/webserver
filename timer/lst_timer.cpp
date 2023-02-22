#include "lst_timer.h"

using namespace std;

//类外定义静态变量
int* Utils::u_pipefd = NULL; 
int Utils::u_epollfd = 0; 


//定时器链表的析构
sort_timer_lst::~sort_timer_lst(){
    timer_list.clear(); //删除所有定时器
}


//向升序链表的合适位置添加定时器，同时保持链表升序
void sort_timer_lst::add_timer(util_timer* timer){
    for(auto it = timer_list.begin(); it != timer_list.end(); ++it){
        util_timer* cur = *it;

        if(cur->expire < timer->expire){ //找到第一个时间小于timer的定时器cur
            timer_list.insert(--it, timer); //插到他前一个迭代器的后面
            break;
        }
    } 
}

//彻底销毁一个定时器
void sort_timer_lst::del_timer(util_timer* timer){
    timer_list.remove(timer); //从链表中移除该定时器 remove函数会移除所有等于value的元素
    delete timer; //析构该定时器并回收空间
}

//调整定时器在链表中的位置，有时一个定时器的内容可能发生变化，就需要改变它在链表中的位置
void sort_timer_lst::adjust_timer(util_timer *timer){

    timer_list.remove(timer); //从链表中移除该定时器
    add_timer(timer); //重新加入链表

}

//将所有超时的定时器从链表中删除，并执行回调函数处理超时连接
void sort_timer_lst::tick(){

    time_t now = time(NULL); //获取现在的时间，一个long long

    list<util_timer*>::iterator it = timer_list.begin();

    while(it != timer_list.end()){
        util_timer* cur = *it;
        if(now < cur->expire) //到达第一个未超时的定时器
            break;
        
        it = timer_list.erase(it); //erase函数返回删除元素的下一个迭代器。erase之后，容器上的迭代器会失效，所以必须让it重新赋值
        //每删除一个迭代器，it会自动移到下一个位置，因此不用我们手动++
        cur->cb_func(cur->user_data); //调用回调处理该超时定时器

        delete cur; //析构该定时器并回收内存
        
    }
}


void Utils::init(int timeslot, int epollfd, int* pipefd){
    m_TIMESLOT = timeslot;

    //使用webserver里的epoll和管道
    u_epollfd = epollfd;
    u_pipefd = pipefd;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd){
//非阻塞：当你去读写一个非阻塞文件描述符的时候，无论是否可读写都会 立即返回结果

    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//向epoll的内核事件表注册一个读事件。该函数在本类中只是用于检测管道的读端。
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
//one_shot代表是否开启EPOLLONESHOT，TRIGMode代表是否采用ET模式（LT模式是缺省模式）

    epoll_event event;
    event.data.fd = fd;

    event.events = EPOLLIN | EPOLLRDHUP; //检测读事件 和对端断开连接
    
    if(TRIGMode == 1) //1代表采用ET模式
        event.events |= EPOLLET;

    if(one_shot) //EPOLLONESHOT
        event.events |= EPOLLONESHOT; 

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event); //把要被检测的事件注册到epoll的内核事件表

    //设置管道读端为非阻塞。ET模式必须搭配非阻塞，而LT模式无所谓
    setnonblocking(fd); 
    
}

//信号处理函数：只是简单地通知主循环程序接收到信号，并把信号值传递给主循环
void Utils::sig_handler(int sig){

    //因为中断之后，处理的过程中可能发生一些错误，errno会设置为其他值，所以我们先把errno保存下来
    /*保留原来的errno，在函数最后恢复，以保证函数的可重入性*/
    int save_errno = errno;

    int msg = sig;

    //将信号值写入管道写端，以通知主循环
    int res = send(u_pipefd[1], (char*)&msg, sizeof(int), 0); 

    errno = save_errno; //复原errno
}

//设置信号处理函数，restart代表是否自动重启被该信号中断的系统调用
void Utils::addsig(int sig, void(handler)(int), bool restart){

    struct sigaction sa; //创建sigaction结构体变量
    memset(&sa, 0, sizeof(sa)); 

    sa.sa_handler = handler; //结构体中的sa_hander成员指定信号处理函数

    if(restart == true)
        sa.sa_flags |= SA_RESTART; //为信号设置SA_RESTART标志以自动重启被该信号中断的系统调用
    
    //将sa_mask全部置为1，表示信号处理函数执行期间，阻塞所有信号,直到信号处理函数执行完毕。防止这些信号被屏蔽
      sigfillset(&sa.sa_mask);

    //设置信号处理函数
    assert(sigaction(sig, &sa, NULL) != -1);
    
}

//定时处理所有超时连接，重新调用alarm启动本进程的定时器。以不断触发SIGALRM信号
void Utils::timer_handler(){
    m_timer_lst.tick(); //处理所有的超时定时器
    alarm(m_TIMESLOT); //每隔m_TIMESLOT时间触发SIGALRM信号
}

//向服务端发送错误信息。用于当连接数超出上限后，向客户端发送错误信息
void Utils::show_error(int connfd, const char *info){
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

//定时器容器类的tick函数，会调用该回调函数来关闭一个超时连接
void cb_func(client_data* user_data){

    assert(user_data);

    //从内核事件表删除非活动连接在socket上的注册事件
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0); 

    //关闭文件描述符
    close(user_data->sockfd); 

    http_conn::m_user_count--; //减少连接数
}