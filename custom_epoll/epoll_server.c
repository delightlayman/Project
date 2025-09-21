#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_EVENTS 64
#define BUF_SIZE 4096

// 自定义连接上下文结构
typedef struct {
    int fd;                 // 连接的文件描述符
    char buf[BUF_SIZE];     // 数据缓冲区
    size_t buf_len;         // 缓冲区中数据长度
    int state;              // 连接状态（0: 读取请求, 1: 发送响应）
    // 可以添加更多字段，如请求解析结果、响应内容等
} connection_t;

// 设置文件描述符为非阻塞模式
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 创建监听socket
int create_listen_socket(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    
    // 设置SO_REUSEADDR选项避免"Address already in use"错误
    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    
    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    set_nonblocking(listen_fd);
    return listen_fd;
}

// 处理新连接
void accept_connection(int epoll_fd, int listen_fd) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd == -1) {
        perror("accept");
        return;
    }
    
    set_nonblocking(client_fd);
    
    // 创建连接上下文
    connection_t *conn = (connection_t*)malloc(sizeof(connection_t));
    if (!conn) {
        perror("malloc");
        close(client_fd);
        return;
    }
    
    // 初始化连接上下文
    memset(conn, 0, sizeof(connection_t));
    conn->fd = client_fd;
    conn->state = 0; // 初始状态为读取请求
    
    // 注册到epoll，关注可读事件，并使用ET模式
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = conn; // 关键：将连接上下文指针存入epoll数据
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
        perror("epoll_ctl");
        free(conn);
        close(client_fd);
        return;
    }
    
    printf("Accepted new connection %d\n", client_fd);
}

// 处理客户端请求
void handle_client_input(int epoll_fd,connection_t *conn) {
    ssize_t count = read(conn->fd, conn->buf + conn->buf_len, 
                         BUF_SIZE - conn->buf_len - 1);
    
    if (count == -1) {
        if (errno != EAGAIN) {
            perror("read");
            return; // 需要关闭连接
        }
        // EAGAIN表示没有更多数据可读
        return;
    } else if (count == 0) {
        // 对端关闭连接
        printf("Connection %d closed by peer\n", conn->fd);
        return; // 需要关闭连接
    }
    
    conn->buf_len += count;
    conn->buf[conn->buf_len] = '\0';
    
    // 检查是否收到完整的HTTP请求（简单检查是否包含空行）
    if (strstr(conn->buf, "\r\n\r\n") != NULL) {
        printf("Received complete request from %d\n", conn->fd);
        
        // 准备响应
        const char *response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Hello, World!";
        
        // 将响应存入缓冲区（实际应用中可能需要动态分配）
        memcpy(conn->buf, response, strlen(response));
        conn->buf_len = strlen(response);
        conn->state = 1; // 切换到发送状态
        
        // 修改epoll监听事件为可写
        struct epoll_event ev;
        ev.events = EPOLLOUT | EPOLLET;
        ev.data.ptr = conn;
        
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) == -1) {
            perror("epoll_ctl mod");
            return; // 需要关闭连接
        }
    }
}

// 发送响应给客户端
void handle_client_output(connection_t *conn) {
    ssize_t count = write(conn->fd, conn->buf, conn->buf_len);
    
    if (count == -1) {
        if (errno != EAGAIN) {
            perror("write");
            return; // 需要关闭连接
        }
        // EAGAIN表示暂时无法写入，等待下次可写事件
        return;
    }
    
    // 更新缓冲区
    if ((size_t)count < conn->buf_len) {
        memmove(conn->buf, conn->buf + count, conn->buf_len - count);
    }
    conn->buf_len -= count;
    
    // 如果所有数据都已发送，关闭连接
    if (conn->buf_len == 0) {
        printf("Response sent to %d, closing connection\n", conn->fd);
        return; // 需要关闭连接
    }
}

// 清理连接资源
void cleanup_connection(int epoll_fd, connection_t *conn) {
    // 从epoll中移除
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    
    // 关闭socket
    close(conn->fd);
    
    // 释放连接上下文内存
    free(conn);
    
    printf("Connection cleaned up\n");
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    int port = atoi(argv[1]);
    int listen_fd = create_listen_socket(port);
    
    // 创建epoll实例
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }
    
    // 添加监听socket到epoll
    struct epoll_event ev;
    ev.events = EPOLLIN; // 监听可读事件（新连接）
    ev.data.fd = listen_fd; // 对于监听socket，使用fd字段足够
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }
    
    printf("Server listening on port %d\n", port);
    
    // 事件循环
    struct epoll_event events[MAX_EVENTS];
    
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            // 处理新连接
            if (events[i].data.fd == listen_fd) {
                accept_connection(epoll_fd, listen_fd);
            } 
            // 处理客户端连接
            else {
                // 关键：从epoll事件中获取我们之前存储的连接上下文
                connection_t *conn = (connection_t *)events[i].data.ptr;
                
                // 检查错误或挂起事件
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    printf("Error or hangup on connection %d\n", conn->fd);
                    cleanup_connection(epoll_fd, conn);
                    continue;
                }
                
                // 处理可读事件
                if (events[i].events & EPOLLIN) {
                    handle_client_input(epoll_fd,conn);
                }
                
                // 处理可写事件
                if (events[i].events & EPOLLOUT) {
                    handle_client_output(conn);
                }
                
                // 检查连接是否需要关闭（在handle_*函数中设置了需要关闭的标志）
                // 这里简化处理，实际应用中可能需要更复杂的状态管理
                if (conn->buf_len == 0 && conn->state == 1) {
                    cleanup_connection(epoll_fd, conn);
                }
            }
        }
    }
    
    close(listen_fd);
    close(epoll_fd);
    return 0;
}