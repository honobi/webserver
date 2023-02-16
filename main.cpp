#include "config.h"

using namespace std;

//使用单例模式的饿汉模式，在程序开始运行时就创建Config对象
Config* Config::config = new Config();

int main(int argc, char* argv[]){
    
    //解析命令行
    Config::get_instance()->parse_arg(argc, argv);
    
    WebServer server;

    //初始化webserver配置参数
    server.init(Config::get_instance());
    
    //是否开启日志
    server.log_write();

    //创建数据库连接池
    server.sql_pool();

    //创建线程池
    server.thread_pool();

    //设置listenfd和http连接fd上事件的触发模式
    server.trig_mode();

    //监听，以及其他准备工作
    server.eventListen();

    //运行
    server.eventLoop();

    return 0;

}


