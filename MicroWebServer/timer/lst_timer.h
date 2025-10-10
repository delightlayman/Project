#ifndef _LST_TIMER_
#define _LST_TIMER_

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
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <ctime>

#include "../log/log.h"

class util_timer;
// 客服端连接信息
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

// 定时器
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;

    void (*cb_func)(client_data *);
    client_data *user_data;
    util_timer *prev;
    util_timer *next;
};

// 定时器 双向链表
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick();

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    // 设置非阻塞
    int setnonblocking(int fd);

    // epoll读事件注册，是否EPOLLONESHOT，是否ET模式
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    // 信号处理函数
    static void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    static int u_epollfd;
    sort_timer_lst m_timer_lst;
    int m_TIMESLOT;
};
// 非活跃连接：客户端与服务器端建立连接后，而长时间不交换数据的连接。
// 非活跃连接弊端：一直占用服务器端的连接资源，如文件描述符，导致资源浪费。
// 回调函数：清理非活跃连接，释放资源。
void cb_func(client_data *user_data);

#endif
