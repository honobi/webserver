#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <mysql/mysql.h>
#include <list>
#include "../locker.h"
#include "../log/log.h"

//连接池是一个懒汉的单例模式，用c++11之后静态局部变量的方式实现
class connection_pool{
private:
    connection_pool();
    ~connection_pool();

    int m_maxconn; //最大连接数
    locker lock;
    std::list<MYSQL*> conn_list; //连接池
    sem reserve; //剩余可用连接数的信号量

public:
    void init(std::string url, std::string User, std::string PassWord, std::string DataBaseName, int Port, int MaxConn, int close_log); 

    MYSQL* get_connection(); 
    bool release_connection(MYSQL *conn); 
    void destroy_pool(); 

    static connection_pool* get_instance(); //单例模式

public:
    std::string m_url;  //主机地址
    std::string m_port; //数据库端口号
    std::string m_user; //数据库用户名
    std::string m_password; //数据库密码
    std::string m_database_name; //数据库名
    int m_close_log; //日志开关

};

//通过RAII机制获取和销毁连接
class connectionRAII{
public:
    connectionRAII(MYSQL** con, connection_pool* connPool);
    ~connectionRAII();
private:
    MYSQL* conRAII; //保存连接的成员变量
    connection_pool* poolRAII; //指向数据库连接池的指针
};
//RAII机制，通过在栈区创建临时变量，让临时变量接管堆上内存的控制权，当该临时变量声明周期结束时，对应的堆上内存自然就被释放了

#endif