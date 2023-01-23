#include <mysql/mysql.h>
#include <list>
#include <stdlib.h> //exit()头文件
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool():m_curconn(0), m_freeconn(0){

}

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
        m_freeconn++; //空闲连接数+1
    }
    reserve = sem(m_freeconn);  //创建完连接后，将成功创建的连接数作为信号量初始值
    m_maxconn = m_freeconn; //将成功创建的连接数作为最大值
}

//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL* connection_pool::get_connection(){

    MYSQL* con = NULL;
    if(conn_list.size() == 0){//如果已经没有可用连接，返回NULL。
        //我认为这样写是错误的，conn_list作为一个共享变量怎么能直接不加锁直接访问
        //其次，我认为这一步还是多余的，真正起作用的是下面的wait，当信号量为0时阻塞住
        return NULL;
    } 

    reserve.wait(); //对信号量P操作，当信号量为0时阻塞住
    lock.lock(); //上锁

    con = conn_list.front(); 
    conn_list.pop_front(); //从空闲连接中取出一个连接

    m_freeconn--;
    m_curconn++; //当前正被使用的连接数+1

    lock.unlock(); //解锁
    return con;

}

//释放当前使用的连接
bool connection_pool::release_connection(MYSQL* con){
    if(con == NULL)
        return false;
    
    lock.lock();

    conn_list.push_back(con);
    m_freeconn++;
    m_curconn--;

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
        m_freeconn = 0;
        m_curconn = 0; //空闲连接数和正在使用连接数清零
        conn_list.clear(); //清空list
        
    }

    lock.unlock();
}

//当前空闲的连接数
int connection_pool::get_free_conn(){
    return this->m_freeconn;
}

//析构
connection_pool::~connection_pool(){
	destroy_pool(); //销毁数据库连接池
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){ 
    //参数SQL是一个二级指针**而不是一级指针*的原因：
    //我们需要改变指针的指向，那么就要传**，如果传*，那么只能改变指针指向的对象本身，而不能改变指向
    *SQL = connPool->get_connection(); //从连接池中获取一个可用连接，并作为传出参数给到*SQL
    //*SQL是一个MYSQL*类型，是一个指针
    conRAII = *SQL;    //将MYSQL*类型的成员变量指向这个连接
    poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->release_connection(conRAII);
}


