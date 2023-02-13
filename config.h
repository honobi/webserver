#ifndef CONFIG_H
#define CONFIG_H

#include "webserver.h"

class Config
{
public:
    
    int PORT;           //端口号
    int LOGWrite;       //日志写入方式
    int TRIGMode;       //触发组合模式
    int LISTENTrigmode; //listenfd触发模式
    int CONNTrigmode;   //connfd触发模式
    int OPT_LINGER;     //是否使用socket的SO_LINGER选项关闭链接
    int sql_num;        //数据库连接池数量
    int thread_num;     //线程池内的线程数量
    int close_log;      //是否关闭日志
    int actor_model;    //并发模型选择

    Config(){
        PORT = 80;      //端口号默认80
        LOGWrite = 0;   //日志写入方式，默认同步
        TRIGMode = 0;   //触发组合模式,默认listenfd LT + connfd LT
        LISTENTrigmode = 0; //listenfd触发模式，默认LT
        CONNTrigmode = 0;   //connfd触发模式，默认LT
        OPT_LINGER = 0; //默认不使用socket的SO_LINGER选项
        sql_num = 8;    //数据库连接池数量,默认8
        thread_num = 8; //线程池内的线程数量,默认8
        close_log = 0;  //默认启用日志
        actor_model = 0;    //默认proactor模式
    }

    ~Config(){};

    //
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