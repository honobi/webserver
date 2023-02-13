#include "config.h"

using namespace std;

int main(int argc, char* argv[]){
    //数据库名，用户名，密码
    string user = "root";
    string databasename = "my_db";
    string passwd = "Hhyybwt2021.";
    
    
    Config config;
    //解析命令行
    config.parse_arg(argc, argv);

    WebServer server;

    //初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, 
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,  config.thread_num, 
                config.close_log, config.actor_model);
    

    //日志
    server.log_write();

    //数据库
    server.sql_pool();

    //线程池
    server.thread_pool();

    //触发模式
    server.trig_mode();

    //监听
    server.eventListen();

    //运行
    server.eventLoop();

    return 0;

}


