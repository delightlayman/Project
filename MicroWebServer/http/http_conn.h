#ifndef _HTTPCONNECTION_H_
#define _HTTPCONNECTION_H_

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

#include <map>
#include <string>
#include <utility>

#include "../lock/locker.h"
#include "../connpool/connectionpool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

using std::map;
using std::pair;
using std::string;

class http_conn
{
public:
    // 读取文件的名称大小
    static const int FILENAME_LEN = 200;
    // 读缓冲区大小
    static const int READ_BUFFER_SIZE = 2048;
    // 写缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;

    // 报文请求方法
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };

    // 主状态机状态---标识报文解析位置
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };

    // 报文解析结果
    enum HTTP_CODE
    {
        // 请求不完整，需要继续读取请求报文数据
        NO_REQUEST,
        // 获得了完整的HTTP请求
        GET_REQUEST,
        // HTTP请求报文有语法错误
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        // 服务器内部错误，主状态机逻辑switch的default情况
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

    // 从状态机状态---解析一行的读取状态
    enum LINE_STATUS
    {
        // 读取行完整
        LINE_OK = 0,
        // 报文语法错误
        LINE_BAD,
        // 读取行未完整
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
              int close_log, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    // 处理任务或请求
    void process();
    // 读取客服端发来的全部数据
    bool read_once();
    // 响应报文写入函数
    bool write();

    sockaddr_in *get_address() { return &m_address; }

    // 初始化数据库读表
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;

private:
    void init();
    // 从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();
    // 写入m_write_buf，并处理响应报文
    bool process_write(HTTP_CODE ret);
    // 主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    // 主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    // 主状态机解析报文中的请求体数据
    HTTP_CODE parse_content(char *text);
    // 生成响应报文
    HTTP_CODE do_request();

    // m_start_line是已经解析的字符
    // get_line用于将指针向后偏移，指向未处理的字符
    char *get_line() { return m_read_buf + m_start_line; };
    // 从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();

    void unmap();

    // 根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state; // 读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;

    // 存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];
    // m_read_buf现有数据长度
    long m_read_idx;
    // m_read_buf当前读取的位置
    long m_checked_idx;
    // m_read_buf中已经解析的字符个数
    int m_start_line;

    // 存储发出的响应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    // 指示buffer中的长度
    int m_write_idx;

    // 主状态机状态
    CHECK_STATE m_check_state;
    // 请求方法
    METHOD m_method;

    // 以下为解析请求报文中对应的6个变量
    // 存储读取文件的名称
    char m_real_file[FILENAME_LEN];
    char *m_url; //仅标识资源路径，而非完整的url格式
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger; // 是否保持连接 linger:留存

    // 读取服务器上的文件地址
    char *m_file_address;
    struct stat m_file_stat;
    // io向量机制iovec
    struct iovec m_iv[2];
    int m_iv_count;
    // 是否启用的POST
    int cgi;
    // 存储请求头数据
    char *m_string;
    // 剩余发送字节数
    int bytes_to_send;
    // 已发送字节数
    int bytes_have_send;

    char *doc_root;
    
    // 是否启用ET模式
    int m_TRIGMode;
    // 是否关闭日志
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
