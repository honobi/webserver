#include "lst_timer.h"

using namespace std;

//类外定义静态变量
int* Utils::u_pipefd = NULL; 
int Utils::u_epollfd = 0; 

void timer_heap::m_swap(int i, int j){
    //交换两个timer
    swap(min_heap[i], min_heap[j]);
    //交换他们在hash表里的下标
    swap(umap[min_heap[i]], umap[min_heap[j]]);
}

void timer_heap::siftup(int i){
    while(i > 0 && min_heap[ (i - 1) / 2]->expire > min_heap[i]->expire){
        m_swap( (i - 1) / 2, i);
        i = (i - 1) / 2;
    }
}
void timer_heap::siftdown(int i){

    while(i < min_heap.size()){
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        int min_index = i;  //保存最小值的下标

        //找到最小值下标
        if(left < min_heap.size() && min_heap[left]->expire < min_heap[min_index]->expire)
            min_index = left;
        if(right < min_heap.size() && min_heap[right]->expire < min_heap[min_index]->expire)
            min_index = right;

        //如果父节点不是三个中最大的
        if(min_index != i){
            m_swap(i, min_index);
            i = min_index;
        }
        //如果父节点是最大的，那么下滤结束
        else
            break;
    }
}

void timer_heap::add_timer(util_timer* timer){
    //timer放进数组
    min_heap.push_back(timer);

    //在hash表里记录该timer的位置
    umap[timer] = min_heap.size() - 1;

    //上滤
    siftup(min_heap.size() - 1);

}

//从堆中删除并销毁定时器
void timer_heap::del_timer(util_timer* timer){
//从堆中删除指定元素也好，弹出堆顶元素也好，都是将该元素与最后一个元素交换，然后下滤到合适位置。
//因为这样可以在不破坏堆结构的情况下删除一个元素，做到O(logn)的时间复杂度
    
    //从哈希表取出下标
    int i = umap[timer];

    //将该元素移到最后并pop出数组，然后从hash表删除 并析构定时器
    m_swap(i, min_heap.size() - 1);
    min_heap.pop_back();
    umap.erase(timer);
    delete timer; 

    //下滤调整堆
    siftdown(i);
}

//当定时器时间改变时，调用该函数下滤调整堆
void timer_heap::adjust_timer(util_timer *timer){

    //取出下标并下滤
    int i = umap[timer];
    siftdown(i);
}

//处理所有超时连接
void timer_heap::tick(){

    if(min_heap.empty())
        return;

     //获取现在的时间
    time_t now = time(NULL);

    util_timer* top = min_heap[0];

    while(!min_heap.empty() && top->expire < now){

        //调用回调处理超时连接
        top->cb_func(top->user_data);

        //从堆中删除并销毁定时器
        del_timer(top);

        top = min_heap[0];

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
    m_timer_heap.tick(); //处理所有的超时连接
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