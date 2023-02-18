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
    int m_curconn; //当前正被使用的连接数
    int m_freeconn; //当前空闲的连接数
    locker lock;
    std::list<MYSQL*> conn_list; //连接池
    sem reserve; //剩余可用连接数的信号量

public:
    //初始化连接池
    void init(std::string url, std::string User, std::string PassWord, std::string DataBaseName, int Port, int MaxConn, int close_log); 

    MYSQL* get_connection(); //获取数据库连接
    bool release_connection(MYSQL *conn); //释放连接
    int get_free_conn(); //获取连接
    void destroy_pool(); //销毁所有连接

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
//RAII机制，通过在栈区创建临时变量，让临时变量接管堆上内存的控制权，当该临时变量声明周期结束时，对应的堆上内存自然就被释放了。
/*这个connectionRAII类其实内容就封装了一个MYSQL的指针，当你需要获取一个连接时，
只需要构造一个connectionRAII局部变量，构造函数内部会调用get_connection从连接池获取一个连接
当局部变量生命周期结束时，自动调用析构函数，析构函数内调用release_connection释放这个连接
这样就不需要我们手动去释放这个连接了
我的评价是不如直接用智能指针，c++的智能指针本身就是RAII机制实现的
*/
#endif