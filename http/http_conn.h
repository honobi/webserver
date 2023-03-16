#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
#include <string.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <fstream>
#include <sys/mman.h>

#include "../locker.h"
#include "../mysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"


class http_conn {
public:
     //请求方法
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH }; 

    //主状态机的状态
    enum CHECK_STATE { 
        CHECK_STATE_REQUESTLINE = 0,    //解析请求行
        CHECK_STATE_HEADER,             //解析请求头
        CHECK_STATE_CONTENT             //解析消息体，仅用于解析POST请求
    };

    //从状态机的状态，标识解析一行的读取状态。
    enum LINE_STATUS{ 
        LINE_OK = 0,    //完整读取一行
        LINE_BAD,       //报文语法有误
        LINE_OPEN       //读取的行不完整
    };

    //报文解析的结果
    enum HTTP_CODE { 
        NO_REQUEST, //请求不完整，需要继续读取请求报文数据
        GET_REQUEST, //获得了完整的HTTP请求
        BAD_REQUEST, //请求报文有语法错误
        NO_RESOURCE, //服务器内部错误
        FORBIDDEN_REQUEST, //请求文件不存在
        FILE_REQUEST,      //没有请求文件的访问权限
        INTERNAL_ERROR,    //请求文件存在，且可以访问
        CLOSED_CONNECTION     //关闭连接
    };
        

    static const int FILENAME_LEN = 200;        //文件名最大长度
    static const int READ_BUFFER_SIZE = 2048;   //读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;  //写缓冲区大小
    static int m_epollfd;       //静态变量，epoll文件描述符
    static int m_user_count;    //静态变量，所有的http连接数
    int shoule_close;     //是否应该关闭该连接。线程池拿到的只有http_conn*，但是关闭连接需要知道他的定时器，因此就用了shoule_be_close在线程池中标记一个连接是否被关闭，然后在webserver类中关闭这个连接
    int have_io;         //在Reactor模式下，标记该连接是否已经在线程池中执行过IO操作。因为IO失败就要立马关闭连接，但是线程池拿到的只有http_conn*，而关闭连接需要知道定时器，所以用这个变量来标记，然后在webserver类中关闭这个连接
    MYSQL *mysql;
    int m_state;  //标记HTTP连接目前是应该 读请求报文 还是 写响应报文。读为0, 写为1

    int http_priority;   //该http请求优先级。图片、视频、交流界面为低优先级，其余的都是高优先级

   
public:
    http_conn(){}
    ~http_conn(){}

    //初始化连接
    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
            int close_log, std::string user, std::string passwd, std::string sqlname);
    void close_conn(bool real_close = true); 
    void process();   
    bool read_once(); 
    bool write();   

    sockaddr_in* get_address(){ return &m_address; }
    void initmysql_result(connection_pool *connPool); 

    bool if_keep_alive();

    
private:
    void init();
    HTTP_CODE process_read();   
    bool process_write(HTTP_CODE ret);      
    HTTP_CODE parse_request_line(char *text);  
    HTTP_CODE parse_headers(char *text);        
    HTTP_CODE parse_content(char *text);        
    HTTP_CODE do_request();                     

    char *get_line() { return m_read_buf + m_start_line; }; //获取指向本行开头的指针
    LINE_STATUS parse_line();       
    void unmap();

    //向响应报文添加内容，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

private:
    int m_sockfd;
    sockaddr_in m_address;

    char m_read_buf[READ_BUFFER_SIZE];  //读缓冲区，存储请求报文数据
    int m_read_idx;                 //指向读缓冲区m_read_buf的 数据末尾的下一个字节
    int m_checked_idx;              //从状态机当前在m_read_buf中读取的位置。
    int m_start_line;               //每一个数据行在m_read_buf中的起始位置

    char m_write_buf[WRITE_BUFFER_SIZE];  //写缓冲区，存储要发出的响应报文
    int m_write_idx;                    //指向写缓冲区m_write_buf的数据末尾的下一个位置，也是当前写缓冲区的长度

    CHECK_STATE m_check_state; //主状态机的状态

    METHOD m_method;          //请求方法

    //以下为解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN];     //存储读取文件的名称
    char* m_url;                        //请求的资源
    char* m_version;                    //HTTP版本
    char* m_host;                       //HTTP请求的目标主机名
    int m_content_length;               //HTTP报文消息体的长度
    bool m_keep_alive;                  //是否为长连接

    char* m_file_address;           //请求的资源的地址，也就是mmap返回的文件指针
    struct stat m_file_stat;        //存放文件信息的结构体，这个文件是请求的资源
    struct iovec m_iv[2];           //聚集写。m_iv[0]存放状态行、响应头、空行，m_iv[1]存放响应体
    int m_iv_count;                 //要使用的iovec个数，在本项目中要么是2要么是1
    int if_post;                    //是否启用的POST
    char* http_body;                //存储请求报文的消息体
    int bytes_to_send;              //剩余发送字节数
    int bytes_have_send;            //已发送字节数
    char* doc_root;                 //网站根目录位置

    int m_TRIGMode;        //http连接上读写事件的触发模式，0代表LT，1代表ET
    int m_close_log;

    char sql_user[100]; 
    char sql_passwd[100];
    char sql_name[100];


    static locker m_lock;    //保护users表的互斥锁
    static std::map<std::string, std::string> users;    //存储全部用户名和密码的map容器

};



#endif