#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include "webserver.h"

class Config
{
private:

    static Config* config;
    Config(){ }
    ~Config(){};

public:
    
    static Config* get_instance(){
        return Config::config;
    }

    //(c++11)为类内数据成员提供一个类内初始值。创建对象时，类内初始值将用于初始化数据成员，没有初始值的成员被默认初始化。类内初始值只能使用=和花括号
    int PORT = 80;           //端口号
    int LOGWrite = 0;       //日志写入方式，默认同步日志
    int TRIGMode = 0;       //触发组合模式，默认listenfd LT + connfd LT
    int LISTENTrigmode = 0; //listenfd触发模式，默认LT
    int CONNTrigmode = 0;   //connfd触发模式，默认LT
    int OPT_LINGER = 0;     //是否使用socket的SO_LINGER选项关闭链接
    int sql_num = 8;        //数据库连接池数量
    int thread_num = 8;     //线程池内的线程数量
    int close_log = 0;      //是否关闭日志
    int actor_model = 0;    //并发模型选择，默认proactor模式

    //数据库名，用户名，密码
    std::string user = "root";
    std::string passwd = "Hhyybwt2021.";
    std::string databasename = "my_db";
    
    void parse_arg(int argc, char*argv[]){
        int opt;
        const char *str = "p:l:m:o:s:t:c:a:";
        //选项后跟一个冒号，表示选项后必须跟一个参数，参数紧跟在选项后或者以空格隔开

        //依次获取所有参数的值
        while ((opt = getopt(argc, argv, str)) != -1)
        {
            switch (opt)
            {
            case 'p':
            {
                PORT = atoi(optarg);
                break;
            }
            case 'l':
            {
                LOGWrite = atoi(optarg);
                break;
            }
            case 'm':
            {
                TRIGMode = atoi(optarg);
                break;
            }
            case 'o':
            {
                OPT_LINGER = atoi(optarg);
                break;
            }
            case 's':
            {
                sql_num = atoi(optarg);
                break;
            }
            case 't':
            {
                thread_num = atoi(optarg);
                break;
            }
            case 'c':
            {
                close_log = atoi(optarg);
                break;
            }
            case 'a':
            {
                actor_model = atoi(optarg);
                break;
            }
            default:
                break;
            }
        }
    }
};

#endif