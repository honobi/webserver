#include "http_conn.h"

using namespace std;

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

locker m_lock;
map<string, string> users; //用户名为key，密码为value

//从数据库user表中取出用户名和密码放到map容器中
void http_conn::initmysql_result(connection_pool* connPool){

    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool); 

    //在user表中检索全部username，passwd数据
    if(mysql_query(mysql, "SELECT username, passwd FROM user")) //成功查询返回0，失败返回非0
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql)); 

    //将上一次查询结果存储到本地的一个MYSQL_RES结构
    MYSQL_RES* result = mysql_store_result(mysql); 

    //检索结果集的下一行，将对应的用户名和密码，存入map中
    while(MYSQL_ROW row = mysql_fetch_row(result)){
        //MYSQL_ROW是一个行结构，类型为char** ，也就是一个字符串数组，每个字符串是一个字段的值

        users[row[0]] = users[row[1]]; 
        //用户名为key，密码为value
    }
    
}

//对文件描述符设置非阻塞
int setnonblocking(int fd){
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
    return old_flag;
}

//向epoll的内核事件表注册一个读事件
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
    //one_shot代表是否开启EPOLLONESHOT，TRIGMode代表是否采用ET模式（LT模式是缺省模式）

    epoll_event event; //指定事件
    event.data.fd = fd; //将要检测的文件描述符作为用户数据，与要检测的事件类型绑定到一个event上

    event.events = EPOLLIN | EPOLLRDHUP; //检测读事件 和对端断开连接

    if(TRIGMode == 1)
        event.events |= EPOLLET;

    if(one_shot)
        event.events |= EPOLLONESHOT; 
    
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    setnonblocking(fd);
    
}

//从内核事件表删除描述符，并关闭文件描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd); 
}

//重置EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//关闭http连接
void http_conn::close_conn(bool real_close){
    if(real_close && m_sockfd != -1){
        printf("close %d\n", m_sockfd); //打印关闭的socket文件描述符
        removefd(m_epollfd, m_sockfd);  //从内核事件表移除对该socket文件描述符的检测
        m_sockfd = -1;  //重置socket文件描述符
        m_user_count--; //连接数-1
    }
}

//初始化连接。外部调用负责初始化socket
void http_conn::init(int sockfd, const sockaddr_in &addr, char* root, int TRIGMode,
                    int close_log, std::string user, std::string passwd, std::string sqlname)
{   
    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空

    m_sockfd = sockfd;
    m_address = addr;
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    //向内核事件表注册对sockfd读事件的检测，启用EPOLLONESHOT
    addfd(m_epollfd, sockfd, true, m_TRIGMode);   
    m_user_count++; //http连接数+1

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());
    
    init();
}

//初始化新连接，设置一些成员变量的初始值
void http_conn::init(){
    
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE; //check_state默认为分析请求行状态
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;  
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);

}

//从状态机，用于分析出一行内容，将\r\n改成\0\0
http_conn::LINE_STATUS http_conn::parse_line(){
    //返回值为行的读取状态，有LINE_OK：完整读取一行；LINE_BAD：报文语法有误；LINE_OPEN：读取的行不完整

    char temp; //存储当前读取到的字节
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        //如果当前是\r字符（回车），则有可能会读取到完整行
        if (temp == '\r') 
        { 
            //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
            if ((m_checked_idx + 1) == m_read_idx) 
                return LINE_OPEN; 
            //下一个字符是\n，将\r\n改为\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n') 
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //如果都不符合，则返回语法错误
            return LINE_BAD;
        }
        //如果当前字符是\n，也有可能读取到完整行：
        //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if (temp == '\n') {
            //前一个字符是\r，则接收完整
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //并没有找到\r\n，需要继续接收
    return LINE_OPEN;

}

//将接收到的数据放进 读缓冲区 中
//ET模式下允许一次调用不读完所有数据；非阻塞ET工作模式下，需要再一次调用内将数据读完
bool http_conn::read_once()
{
    //已读完
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0; 

    //LT模式读取数据
    if (m_TRIGMode == 0)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read; //移动已读指针

        if (bytes_read <= 0) //recv出错时返回-1并设置errno
            return false;
        
        return true;
    }
    //ET模式读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) //recv出错时返回-1并设置errno
            {   
                //因为ET配合的文件描述符是非阻塞的，而非阻塞的系统调用经常会出现EAGAIN，即再试一次
                //在此处就是：现在没有数据可读请稍后再试
                if (errno == EAGAIN || errno == EWOULDBLOCK) //一些系统上EAGAIN的名字叫做EWOULDBLOCK
                    break;
                return false;
            }
            else if (bytes_read == 0) //recv返回0，意味着通信对方已经关闭连接了
            {
                return false;
            }
            m_read_idx += bytes_read; //移动已读指针
        }
        return true;
    }
}

//解析http请求行，获得请求方法、目标url、http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    //查找空格或\t在text中第一次出现的位置
    m_url = strpbrk(text, " \t");  //strpbrk功能是查找第二个字符串中 任何字节 在第一个字符串中的第一次出现的位置
    
    //如果没有空格或\t，则报文格式有误
    if (!m_url)
        return BAD_REQUEST;
    
    //在原本空格或\t的位置插入一个\0，就可以提取出请求方法
    *m_url++ = '\0';    
    char* method = text;

    //判断是GET还是POST请求
    if (strcasecmp(method, "GET") == 0) //strcasecmp函数忽略大小写比较俩字符串，若匹配则返回0
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;

    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url, " \t"); //strspn计算m_url的初始段的长度，该段完全由" \t"中的字节组成
    m_version = strpbrk(m_url, " \t");  //查找空格或\t在m_url中第一次出现的位置
    
    if (!m_version)
        return BAD_REQUEST;

    *m_version++ = '\0';
    m_version += strspn(m_version, " \t"); //strspn计算m_version的初始段的长度，该段完全由" \t"中的字节组成

    //仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    //对请求资源前7个字符进行判断
    //有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/'); //返回'/'第一次出现位置
    }

    //同样增加https情况
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/'); //返回'/'第一次出现位置
    }

    //一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    //当url为/时，显示欢迎界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    //请求行解析完毕，主状态机切换到 解析请求头 状态
    m_check_state = CHECK_STATE_HEADER;

    return NO_REQUEST; //解析完请求行后，请求还不完整，需要继续读取请求报文数据
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    //判断是空行还是请求头
    if (text[0] == '\0')
    {   
        //判断是否有消息体
        //m_content_length初始值为0，如果变得不为0，说明在头部字段中解析出了Content-Length字段
        if (m_content_length != 0)
        {   
            m_check_state = CHECK_STATE_CONTENT;    //主状态机切换到解析消息体
            return NO_REQUEST;  
        }

        //如果没有Content-Length字段，说明该请求没有消息体，那么解析完毕
        return GET_REQUEST; 
    }

    //如果该头部字段是Connection
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");

        //使用keep-alive模式（长连接）
        if (strcasecmp(text, "keep-alive") == 0)
            m_linger = true;
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);  //将消息体的长度 赋值给m_content_length成员
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;  //目标主机名
    }
    else  //其他头部字段 ，就并不打算解析了
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST; 
}

//解析整个消息体
http_conn::HTTP_CODE http_conn::parse_content(char* text)
//参数text指向消息体的开头
{
    //get请求一般没有消息体，参数放在url中。如果buffer中读取了消息体，说明是POST请求。POST请求中最后为输入的用户名和密码 
    
    if (m_read_idx >= m_content_length + m_checked_idx) //我觉得最多只会等于，不会大于
    {
        text[m_content_length] = '\0';  //在消息体的末尾加上\0
        m_string = text; //将整个消息体(\0结尾的字符串)的开头指针赋值给m_string
        return GET_REQUEST;
    }
    //如果m_read_idx < m_content_length + m_checked_idx，说明消息体没读完整
    return NO_REQUEST;
}

//解析整个请求报文
http_conn::HTTP_CODE http_conn::process_read()
{
    //初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = NULL;

    //循环读取HTTP请求的每一行
    //如果主状态机是解析消息体，那么不调用parse_line，否则调用parse_line将行尾的\r\n改成\0\0
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line(); //text指向这一行的开头
        //如果当前解析的是消息体，那么text是整个消息体
        //如果当前解析的不是消息体，那么由于调用了parse_line，会把本行末尾\r\n改为\0\0，此时text就是这一行

        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx是从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;

        LOG_INFO("%s", text); //写入日志

        switch (m_check_state)
        {
        //主状态机状态为解析请求行，解析这行请求行
        case CHECK_STATE_REQUESTLINE:  
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        //主状态机状态为解析请求头，那么解析请求头的这一行
        case CHECK_STATE_HEADER:       
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST) //如果读取到完整请求，就处理请求
                return do_request();
            break;
        }
        //主状态机状态为解析消息体，那么解析 整个 消息体
        case CHECK_STATE_CONTENT:      
        {
            ret = parse_content(text); 
            if (ret == GET_REQUEST)//如果读取到完整请求，就处理请求
                return do_request(); 

            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

//将 被请求的资源 放进共享内存
http_conn::HTTP_CODE http_conn::do_request()
{
    //将初始化的m_real_file赋值为网站根目录doc_root
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    //找到m_url中倒数第一个/的位置
    const char* p = strrchr(m_url, '/');

    //如果是POST请求，并且是/2或/3
    // /2表示登录请求，需要检测用户名密码是否正确，如果正确，那么把m_url赋值为/welcome.html
    // /3表示注册请求，需要检测用户名是否存在，如果不存在，就注册该用户，并把m_url赋值为/log.html
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        char* m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); //将/2/3后面的部分拼接到m_real_file后面
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //如果是注册，先检测数据库中是否有重名的，如果没有重名，向user表和map中插入该用户
        if (*(p + 1) == '3')
        {
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            //明明可以使用sprintf一行解决：
            //sprintf(sql_insert, "INSERT INTO user(username, passwd) VALUES(%s, %s)", name, password);
            
            //如果map容器中没有该用户，那么向数据库该表中插入该用户，插入操作也同步到map容器中（map与user表是始终同步的）
            if (users.find(name) == users.end())
            {
                //使用数据库连接需要上锁，存储用户信息的map容器users也是共享资源，上锁
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert( { name, password } );
                m_lock.unlock();

                if (res == 0) //sql语句执行成功
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            //如果已有该用户命，那么不能再注册
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录
        else if (*(p + 1) == '2')
        {
            //用户名和密码正确
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    //如果请求资源为/0，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        //我不理解为什么要一直申请一块动态内存再释放掉，直接用临时变量不行吗
        char* m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real)); //将/register.html拼接到根目录路径后面

        free(m_url_real);
    }
    //如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real)); //将/log.html拼接到根目录路径后面

        free(m_url_real);
    }
    //如果请求资源为/5，表示图片
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/6，表示视频
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/7，表示关注界面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果以上均不符合，直接将url与网站根目录拼接
    //这里的情况是welcome界面，请求服务器上的一个图片
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
     /*参数含义：0：由系统分配一个地址。m_file_stat.st_size：内存大小
            PROT_READ：内存段访问权限为可读。
            MAP_PRIVATE：内存段为调用进程所私有。对该内存段的修改不会反映到被映射的文件中
            fd：被映射文件对应的文件描述符
            0：偏移量，从文件的何处开始映射，这里0表示整个文件
    */
    close(fd);
    return FILE_REQUEST;
}

//释放由mmap创建的共享内存
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//将写缓冲区中的响应报文发送给浏览器端
bool http_conn::write()
//write函数与作者写的文章06 http连接处理（下）不同，他说书中原代码的write函数不严谨，在文章中对Bug进行了修复，可以正常传输大文件，之后可以看一下
{
    int temp = 0;

    //若要发送的数据长度为0，表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
    while (1)
    {
        //writev函数一次写入的字节数由iovec结构体的iov_len成员决定
        //writec函数写入的顺序是数组的顺序，也就是m_iv[0]、m_iv[1]...这个顺序
        temp = writev(m_sockfd, m_iv, m_iv_count);
        /*
            struct iovec {
                void* iov_base; //指向一个缓冲区，这个缓冲区是存放readv所接收的数据或writev将要发送的数据。   
                size_t iov_len; //确定了接收的最大长度以及实际写入的长度。
            };
        */
        /*参数：
        - fd：要在其上进行读或是写的文件描述符
		- iov：读或写所用的I/O向量；
		- iovcnt：要使用的向量元素个数。
	    - 返回值：成功：readv所读取的字节数或writev所写入的字节数；
			    错误：返回-1，设置errno
        */

        //wirtev失败
        if (temp < 0)
        {
            //缓冲区满，则重置EPOLLONESHOT事件
            if (errno == EAGAIN) 
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode); 
                return true;
            }

            //不是缓冲区满，释放由mmap创建的共享内存
            unmap(); 
            return false;
        }

        //更新已发送字节数和待发送字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;

        
        //每次成功调用writev写入一些字节后，就需要更新m_iv[0]或m_iv[1]的待发送数据的起始地址*/
        //如果writev下次该发送的是m_iv[1]
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //如果writev下次该发送的是m_iv[0]
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //数据已全部发送完
        if (bytes_to_send <= 0)
        {   
            unmap();//取消mmap映射
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);//重置EPOLLONESHOT事件

            //如果浏览器的请求为长连接，重新初始化HTTP对象
            if (m_linger)
            {
                init();
                return true;
            }
            else
                return false;
        }
    }
}

//向写缓冲区中添加响应体的内容，写响应体的每部分都会调用这个函数，为的是代码的复用性
bool http_conn::add_response(const char *format, ...)
{
    //如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    //定义可变参数列表
    va_list arg_list;

    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);

    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }

    //更新m_write_idx位置，m_write_idx指向下一个待写位置，也是当前写缓冲区长度
    m_write_idx += len;

    //清空可变参列表
    va_end(arg_list);

    //写日志
    LOG_INFO("request:%s", m_write_buf);

    return true;
}

//向写缓冲区写入状态行：http/1.1 状态码 状态消息
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//添加响应头和空行，调用三个函数分别添加两个头部字段Content-Length、Connection和空行
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_blank_line();

}

//向写缓冲区写入头部字段Content-Length
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

//向写缓冲区写入头部字段 Content-Type，也就是文档类型
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
    //text是主文档类型，html是子文档类型，text/html表示目标文档（例如index.html）是text类型中的html文档
}

//向写缓冲区写入Connection字段，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

//向写缓冲区写入空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

//向写缓冲区写入响应体
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

//将完整的响应报文写入写缓冲区
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    //服务器内部错误，500
    case INTERNAL_ERROR:
    {
        //状态行
        add_status_line(500, error_500_title);
        //响应头和空行
        add_headers(strlen(error_500_form));
        //响应体
        if (!add_content(error_500_form))
            return false;
        break;
    }

    //报文语法有误，400
    case BAD_REQUEST:
    {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
            return false;
        break;
    }

    //资源没有访问权限，403
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }

    //资源未找到，404
    case NO_RESOURCE:
    {
        add_status_line(400, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }

    //资源存在且可以访问，200
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);

        //如果请求的资源大小不为0
        if (m_file_stat.st_size != 0)
        {
            //向写缓冲区写入响应头和空行
            add_headers(m_file_stat.st_size);

            //m_iv[0]的资源指针 指向 写缓冲区，长度为m_write_idx
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;

            //m_iv[1]的资源指针 指向 mmap返回的文件指针，长度为文件的字节数
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;

            //两个iovec结构体
            m_iv_count = 2;

            //需要发送的数据大小为  状态行、响应头、空行大小 + 文件大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }

        //如果请求的资源大小为0，则返回空白html文件
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    //其他状态
    default:
        return false;
    }

    //除FILE_REQUEST状态有响应体外，其余状态没有响应体，只申请一个iovec，指向写缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//浏览器端发出http连接请求，服务器端主线程创建http对象接收请求并将所有数据读入对应buffer，将该对象插入任务队列后，工作线程从任务队列中取出一个任务进行处理，各工作线程通过process函数对任务进行处理
//解析读缓冲区中的请求报文，然后将响应报文写入写缓冲区
void http_conn::process()
{
    //调用process_read读取并解析报文
    HTTP_CODE read_ret = process_read();

    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);  //重置EPOLLONESHOT
        return;
    }

    //调用process_write完成报文响应
    bool write_ret = process_write(read_ret);

    if (!write_ret)
    {
        close_conn();
    }

    //重置EPOLLONESHOT
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

