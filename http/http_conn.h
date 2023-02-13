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
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"


class http_conn{
public:
     //请求方法，本项目只用到GET和POST
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH }; 

    //主状态机的状态
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    //三种状态，标识解析位置。
        //CHECK_STATE_REQUESTLINE，解析请求行
        //CHECK_STATE_HEADER，解析请求头
        //CHECK_STATE_CONTENT，解析消息体，仅用于解析POST请求

    //从状态机的状态
    enum LINE_STATUS{ LINE_OK = 0, LINE_BAD, LINE_OPEN };
    //三种状态，标识解析一行的读取状态。
        //LINE_OK，完整读取一行
        //LINE_BAD，报文语法有误
        //LINE_OPEN，读取的行不完整

    //报文解析的结果
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
        //NO_REQUEST：请求不完整，需要继续读取请求报文数据
        //GET_REQUEST：获得了完整的HTTP请求
        //BAD_REQUEST：HTTP请求报文有语法错误
        //INTERNAL_ERROR：服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        //NO_RESOURCE：请求文件不存在
        //FORBIDDEN_REQUEST：没有请求文件的访问权限
        //FILE_REQUEST：请求文件存在，且可以访问
        //CLOSED_CONNECTION：

    static const int FILENAME_LEN = 200;        //文件名最大长度
    static const int READ_BUFFER_SIZE = 2048;   //读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;  //写缓冲区大小
    static int m_epollfd;       //静态变量，所有http连接共用一个epoll去检测socket是否可读
    static int m_user_count;    //静态变量，所有的http连接数
    int timer_flag;     //是否应该关闭该连接。当对该连接的IO操作出错时，该请求就无法处理，就可以把它当做超时连接去关闭掉
    int improv;         //标记该连接是否已经在工作线程中执行过IO操作，仅在Reactor模式下使用
    MYSQL *mysql;
    int m_state;  //标记HTTP连接目前是应该 读请求报文 还是 写响应报文。读为0, 写为1

   
public:
    http_conn(){}
    ~http_conn(){}

    //初始化连接
    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
            int close_log, std::string user, std::string passwd, std::string sqlname);
    void close_conn(bool real_close = true); //关闭连接
    void process();   //读请求报文，写响应报文
    bool read_once(); //读取浏览器端发来的全部数据
    bool write();   //响应报文写入函数

    sockaddr_in* get_address(){ return &m_address; }
    void initmysql_result(connection_pool *connPool); //同步线程初始化数据库读取表

    


private:
    void init();
    HTTP_CODE process_read();   //从m_read_buf读取，并处理请求报文
    bool process_write(HTTP_CODE ret);      //向m_write_buf写入响应报文数据
    HTTP_CODE parse_request_line(char *text);   //主状态机解析报文中的请求行数据
    HTTP_CODE parse_headers(char *text);        //主状态机解析报文中的请求头数据
    HTTP_CODE parse_content(char *text);        //主状态机解析报文中的请求内容
    HTTP_CODE do_request();                     //生成响应报文

    char *get_line() { return m_read_buf + m_start_line; }; //获取指向本行开头的指针
    LINE_STATUS parse_line();       //从状态机读取一行，分析是请求报文的哪一部分
    void unmap();

    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
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

    char m_write_buf[WRITE_BUFFER_SIZE];  //存储发出的响应报文数据
    int m_write_idx;                    //指向写缓冲区m_write_buf的数据末尾的下一个位置，也是当前写缓冲区的长度

    CHECK_STATE m_check_state; //主状态机的状态

    METHOD m_method;          //请求方法

    //以下为解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN];     //存储读取文件的名称
    char* m_url;    //请求的资源
    char* m_version;   //HTTP版本
    char* m_host;   //HTTP请求的目标主机名
    int m_content_length;   //HTTP报文消息体的长度
    bool m_linger;  //是否为长连接

    char *m_file_address;           //请求的资源的地址，也就是mmap返回的文件指针
    struct stat m_file_stat;        //存放文件信息的结构体，这个文件是请求的资源
    struct iovec m_iv[2];           //m_iv[0]存放状态行、响应头、空行，m_iv[1]存放响应体
    int m_iv_count;         //要使用的iovec个数，在本项目中要么是2要么是1
    int cgi;                    //是否启用的POST
    char* m_string;             //存储消息体
    int bytes_to_send;          //剩余发送字节数
    int bytes_have_send;         //已发送字节数
    char* doc_root;         //网站根目录位置

    std::map<std::string, std::string> m_users;  //存放用户名和密码的map容器
    int m_TRIGMode;        //http连接上读写事件的触发模式，0代表LT，1代表ET
    int m_close_log;

    char sql_user[100]; 
    char sql_passwd[100];
    char sql_name[100];

};



#endif