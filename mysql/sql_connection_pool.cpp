#include <mysql/mysql.h>
#include <list>
#include <stdlib.h> 
#include <assert.h>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool(){ }

connection_pool* connection_pool::get_instance(){
    static connection_pool connPool;
    return &connPool;
}

void connection_pool::init(string url, string user, string password, string DBName, int port, int maxConn, int close_log){
    m_url = url;
	m_port = port;
	m_user = user;
	m_password = password;
	m_database_name = DBName;
	m_close_log = close_log;

    //创建MaxConn条数据库连接
    for(int i = 0; i < maxConn; ++i){
        MYSQL* con = NULL;
        con = mysql_init(con);

        if(con == NULL){ //初始化失败
            LOG_ERROR("Mysql Error");
            exit(1);
        }
        con = mysql_real_connect(con, url.c_str(), user.c_str(), password.c_str(), DBName.c_str(), port, NULL, 0);

        if(con == NULL){ //连接失败
            LOG_ERROR("Mysql Error");
            exit(1);
        }
        conn_list.push_back(con); //连接成功，那么将连接放进list

    }
    //一个连接也没创建就退出进程
    assert(conn_list.size() > 0);

    reserve = sem(conn_list.size());  //创建完连接后，将成功创建的连接数作为信号量初始值
    m_maxconn = conn_list.size(); //将成功创建的连接数作为最大值
}

//当有请求时，从数据库连接池中取出一个可用连接
MYSQL* connection_pool::get_connection(){

    MYSQL* con = NULL; 

    reserve.wait(); //对信号量P操作，当信号量为0时阻塞住
    lock.lock(); //上锁

    //从空闲连接中取出一个连接
    con = conn_list.front(); 
    conn_list.pop_front(); 

    lock.unlock(); //解锁
    return con;

}

//释放当前使用的连接：把他放回到空闲连接队列
bool connection_pool::release_connection(MYSQL* con){
    if(con == NULL)
        return false;
    
    lock.lock();

    conn_list.push_back(con);

    lock.unlock();

    reserve.post();  //信号量V操作
    return true;
}

//销毁数据库连接池
void connection_pool::destroy_pool(){
    /*这个函数只把list里面空闲连接给关闭了，但是正在被使用的连接由于申请的时候被pop出去，并没在list里面
        也就是说正在被使用的连接没有被关闭，毕竟我总不能把还在用的连接给强行关了吧。
        但是无需担心，因为我们使用RAII机制获取和销毁连接，那些正在被使用的连接，当被使用完后也会被关闭
    */

    lock.lock();

    if(conn_list.size() > 0){
        for(auto it = conn_list.begin(); it != conn_list.end(); ++it){
            MYSQL* con = *it;
            mysql_close(con); //关闭list中所有连接
        }
        conn_list.clear(); //清空list
    }

    lock.unlock();
}


//析构
connection_pool::~connection_pool(){
	destroy_pool(); //销毁数据库连接池
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){ 
    //参数SQL是一个二级指针**而不是一级指针*：我们需要改变指针的指向

    //从连接池中获取一个可用连接，并作为传出参数给到*SQL。*SQL是一个MYSQL*类型，是一个指针
    *SQL = connPool->get_connection(); 

    conRAII = *SQL;    //将MYSQL*类型的成员变量指向这个连接
    poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->release_connection(conRAII);
}


