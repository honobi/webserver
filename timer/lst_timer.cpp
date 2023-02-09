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
#include "lst_timer.h"
#include "../http/http_conn.h"

using namespace std;

//类外定义静态变量
int* Utils::u_pipefd = 0; 
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


void Utils::init(int timeslot){
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd){
//非阻塞：当你去读写一个非阻塞文件描述符的时候，无论是否可读写都会 立即返回结果

    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//向epoll的内核事件表注册一个读事件，让内核去检测管道的写端
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

    setnonblocking(fd); 
    /*为什么要讲管道的写端设置为非阻塞：send是将信息发送给套接字缓冲区，如果缓冲区满了，
        则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞。
    没有对非阻塞返回值处理，如果阻塞是不是意味着这一次定时事件失效了？
        是的，但定时事件是非必须立即处理的事件，可以允许这样的情况发生。
    */
}

/*信号是一种异步事件:信号处理函数和程序的主循环是两条不同 的执行路线。很显然，信号处理函数需要尽可能快地执行完毕，
以确 保该信号不被屏蔽(前面提到过，为了避免一些竞态条件，信号在处 理期间，系统不会再次触发它)太久。
一种典型的解决方案是:把信号的主要处理逻辑放到程序的主循环中，当信号处理函数被触发时， 它只是简单地通知主循环程序接收到信号，
并把信号值传递给主循环，主循环再根据接收到的信号值执行目标信号对应的逻辑代码。
信号处理函数通常使用管道来将信号“传递”给主循环:信号处理函数往 管道的写端写入信号值，主循环则从管道的读端读出该信号值。
那么主循环怎么知道管道上何时有数据可读呢?这很简单，我们只需要使用 I/O复用系统调用来监听管道的读端文件描述符上的可读事件。
如此一来，信号事件就能和其他I/O事件一样被处理，即统一事件源。
*/

//信号处理函数：只是简单地通知主循环程序接收到信号，并把信号值传递给主循环
void Utils::sig_handler(int sig){

    //因为中断之后，处理的过程中可能发生一些错误，errno会设置为其他值，所以我们先把errno保存下来
    /*保留原来的errno，在函数最后恢复，以保证函数的可重入性*/
    int save_errno = errno;

    int msg = sig;
    send(u_pipefd[1], (char*)msg, 1, 0); //将信号值写入管道，以通知主循环
    //这里是用socket的send函数向管道的写端写入数据。
    //两点不理解：读写管道为什么不用read和write
    //          就算用send，那么一个int的长度也是4字节，这里怎么写1
    //他这几个函数都抄的游双书，不知道为什么是1

    errno = save_errno; //复原errno
}

//设置信号处理函数
void Utils::addsig(int sig, void(handler)(int), bool restart){

    struct sigaction sa; //创建sigaction结构体变量
    memset(&sa, 0, sizeof(sa)); //把内存清零

    sa.sa_handler = handler; //结构体中的sa_hander成员指定信号处理函数

    if(restart == true)
        sa.sa_flags |= SA_RESTART; //为信号设置SA_RESTART标志以自动重启被该信号中断的系统调用
    
    sigfillset(&sa.sa_mask); //清空我们自己定义的信号集

    assert(sigaction(sig, &sa, NULL) != -1);
    //sigaction函数：设置信号处理。 第一个参数是要捕获的信号类型，第二个参数指定信号处理方式， 第三个参数则输出信号先前的处理方式
    /*assert：意思：明确肯定、断言。void assert( int expression );
     assert的作用是先计算表达式expression，如果其值为假，那么它先向stderr(标准错误)打印一条出错信息，然后通过调用abort来终止程序运行 */

}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler(){
    m_timer_lst.tick(); //处理所有的超时定时器
    alarm(m_TIMESLOT); //每隔m_TIMESLOT时间触发SIGALRM信号

    /*alarm:设置定时器，单位秒。函数调用的时候开始计时，结束时会给 当前进程 发送一个SIGALARM信号 
      这个定时器是linux的定时器，每个进程只有一个定时器。跟我们定义的定时器类util_timer不是一个东西
      alarm的返回值是之前的定时器剩余的时间 */
}

void Utils::show_error(int connfd, const char *info){
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

//定时器容器类的tick函数，会调用该回调函数来处理一个超时连接
void cb_func(client_data* user_data){

    //从内核事件表删除非活动连接在socket上的注册事件
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0); 

    assert(user_data);  //user_data为NULL时终止程序，因为下面的close要用到user_data
    close(user_data->sockfd); //关闭文件描述符

    http_conn::m_user_count--; //减少连接数
}





