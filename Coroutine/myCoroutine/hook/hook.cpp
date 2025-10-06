#include "hook.hpp"
#include "../iomanager/ioscheduler.hpp"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager.hpp"
#include <string.h>

// apply XX to all functions
#define HOOK_FUN(XX) \
    XX(sleep)        \
    XX(usleep)       \
    XX(nanosleep)    \
    XX(socket)       \
    XX(connect)      \
    XX(accept)       \
    XX(read)         \
    XX(readv)        \
    XX(recv)         \
    XX(recvfrom)     \
    XX(recvmsg)      \
    XX(write)        \
    XX(writev)       \
    XX(send)         \
    XX(sendto)       \
    XX(sendmsg)      \
    XX(close)        \
    XX(fcntl)        \
    XX(ioctl)        \
    XX(getsockopt)   \
    XX(setsockopt)

namespace myCoroutine
{

    // if this thread is using hooked function
    static thread_local bool t_hook_enable = false;

    bool is_hook_enable()
    {
        return t_hook_enable;
    }

    void set_hook_enable(bool flag)
    {
        t_hook_enable = flag;
    }

    // 初始化hook
    void hook_init()
    {
        static bool is_inited = false;
        if (is_inited)
        {
            return;
        }

        // test
        is_inited = true;

// assignment -> sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep");
// dlsym : fetch the original symbols/function
// XX宏定义
// name_f系列函数指针，指向对应的原始函数
#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);
        HOOK_FUN(XX)
// 取消XX的宏定义
#undef XX
    }

    // static variable initialisation will run before the main function
    struct HookIniter
    {
        HookIniter()
        {
            hook_init();
        }
    };

    // 创建hook初始化器，并完成对hook的初始化
    static HookIniter s_hook_initer;

} // end namespace myCoroutine

// 跟踪定时器状态：cancelled -> 0:未取消，1:已取消
struct timer_info
{
    int cancelled = 0;
};

// universal template for read and write function
// 变参模板函数
/*
    fd: 文件描述符
    fun: 原始函数
    hook_fun_name: 原始函数名/hook函数名
    event:事件类型---无/读/写
    timeout_so: 超时时间类型
    ...args:变参   万能引用&& + std::forward -->完美转发
*/
template <typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char *hook_fun_name, uint32_t event, int timeout_so, Args &&...args)
{
    // hook未启用，调用原始函数
    if (!myCoroutine::t_hook_enable)
    {
        return fun(fd, std::forward<Args>(args)...);
    }
    // 获取fd对应的上下文
    std::shared_ptr<myCoroutine::FdCtx> ctx = myCoroutine::FdMgr::GetInstance()->get(fd);
    // 如果上下文不存在，调用原始函数
    if (!ctx)
    {
        return fun(fd, std::forward<Args>(args)...);
    }
    // 若fd已关闭，设置错误码为EBADF，返回-1
    //  EBADF：文件描述符无效(bad fd)
    if (ctx->isClosed())
    {
        errno = EBADF;
        return -1;
    }
    // 若fd非socket，或者用户设置fd为非阻塞，调用原始函数
    if (!ctx->isSocket() || ctx->getUserNonblock())
    {
        return fun(fd, std::forward<Args>(args)...);
    }

    // get the timeout
    uint64_t timeout = ctx->getTimeout(timeout_so);
    // timer condition
    std::shared_ptr<timer_info> tinfo(new timer_info);

retry:
    // run the function
    // 尝试调用原始函数，根据返回值的情况，进行不同的处理
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    // EINTR ->Operation interrupted by system ->retry
    // 信号中断（EINTR）导致的fun执行失败，重试
    while (n == -1 && errno == EINTR)
    {
        n = fun(fd, std::forward<Args>(args)...);
    }

    // 0 resource was temporarily unavailable -> retry until ready
    // 资源暂时不可用（EAGAIN），重试直到资源可用
    if (n == -1 && errno == EAGAIN)
    {
        myCoroutine::IOManager *iom = myCoroutine::IOManager::getThis();
        // timer
        std::shared_ptr<myCoroutine::Timer> timer;
        std::weak_ptr<timer_info> winfo(tinfo);
        // 若设置了定时器，则添加一个条件定时器
        // 1 timeout has been set -> add a conditional timer for canceling this operation
        if (timeout != (uint64_t)-1)
        {
            timer = iom->addConditionTimer(timeout, [winfo, fd, iom, event]()
                                           {
                auto t = winfo.lock();//返回一个指向被观察对象的 std::shared_ptr
                if(!t || t->cancelled) //tinfo已释放 或 已取消
                {
                    return;
                }
                // ETIMEDOUT:超时错误码，标记定时器已取消
                t->cancelled = ETIMEDOUT;
                // cancel this event and trigger once to return to this fiber
                // cancelEvent: 取消事件，并触发一次事件---其中resume()恢复位置：下方yield()
                iom->cancelEvent(fd, (myCoroutine::IOManager::Event)(event)); }, winfo);
        }
        // 不管是否设置定时器，都添加对应事件到fd
        // 2 add event -> callback is this fiber
        int rt = iom->addEvent(fd, (myCoroutine::IOManager::Event)(event));
        //若未添加成功，则清除计时器
        if (rt)
        {
            std::cout << hook_fun_name << " addEvent(" << fd << ", " << event << ")";
            if (timer)
            {
                timer->cancel();
            }
            return -1;
        }
        else
        {
            // 在此终止，后续也在此恢复
            // 恢复：定时器超时 或 事件触发
            myCoroutine::Fiber::getThis()->yield();

            // 3 resume either by addEvent or cancelEvent
            // 事件处理过后，清除计时器
            if (timer)
            {
                timer->cancel();
            }
            // by cancelEvent
            if (tinfo->cancelled == ETIMEDOUT)
            {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    }
    return n;
}

extern "C"
{

// declaration -> sleep_fun sleep_f = nullptr;
#define XX(name) name##_fun name##_f = nullptr;
    HOOK_FUN(XX)
#undef XX

    // only use at task fiber
    unsigned int sleep(unsigned int seconds)
    {
        // hook未启用，调用原始函数
        if (!myCoroutine::t_hook_enable)
        {
            return sleep_f(seconds);
        }

        std::shared_ptr<myCoroutine::Fiber> fiber = myCoroutine::Fiber::getThis();
        myCoroutine::IOManager *iom = myCoroutine::IOManager::getThis();
        // add a timer to reschedule this fiber
        iom->addTimer(seconds * 1000, [fiber, iom]()
                      { iom->addscheduletask(fiber, -1); });
        // wait for the next resume
        // fiber上下文会在yield()中更新为当前上下文，然后切换到调度器上下文
        fiber->yield();
        return 0;
    }

    int usleep(useconds_t usec)
    {
        if (!myCoroutine::t_hook_enable)
        {
            return usleep_f(usec);
        }

        std::shared_ptr<myCoroutine::Fiber> fiber = myCoroutine::Fiber::getThis();
        myCoroutine::IOManager *iom = myCoroutine::IOManager::getThis();
        // add a timer to reschedule this fiber
        iom->addTimer(usec / 1000, [fiber, iom]()
                      { iom->addscheduletask(fiber); });
        // wait for the next resume
        fiber->yield();
        return 0;
    }

    int nanosleep(const struct timespec *req, struct timespec *rem)
    {
        if (!myCoroutine::t_hook_enable)
        {
            return nanosleep_f(req, rem);
        }

        int timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000;

        std::shared_ptr<myCoroutine::Fiber> fiber = myCoroutine::Fiber::getThis();
        myCoroutine::IOManager *iom = myCoroutine::IOManager::getThis();
        // add a timer to reschedule this fiber
        iom->addTimer(timeout_ms, [fiber, iom]()
                      { iom->addscheduletask(fiber, -1); });
        // wait for the next resume
        fiber->yield();
        return 0;
    }

    int socket(int domain, int type, int protocol)
    {
        if (!myCoroutine::t_hook_enable)
        {
            return socket_f(domain, type, protocol);
        }

        int fd = socket_f(domain, type, protocol);
        if (fd == -1)
        {
            std::cerr << "socket() failed:" << strerror(errno) << std::endl;
            return fd;
        }
        // 加入到fdManager中
        myCoroutine::FdMgr::GetInstance()->get(fd, true);
        return fd;
    }

    int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms)
    {
        if (!myCoroutine::t_hook_enable)
        {
            return connect_f(fd, addr, addrlen);
        }

        std::shared_ptr<myCoroutine::FdCtx> ctx = myCoroutine::FdMgr::GetInstance()->get(fd);
        if (!ctx || ctx->isClosed())
        {
            errno = EBADF;
            return -1;
        }
        // 非socket不可connect  connect()会返回-1且errno 设置 ENOTSOCK---文件描述符不指向套接字
        // if (!ctx->isSocket())
        // {
        //     return connect_f(fd, addr, addrlen);
        // }

        if (ctx->getUserNonblock())
        {

            return connect_f(fd, addr, addrlen);
        }

        // attempt to connect
        int n = connect_f(fd, addr, addrlen);
        if (n == 0)
        {
            return 0;
        }
        else if (n != -1 || errno != EINPROGRESS)
        {
            return n;
        }

        // wait for write event is ready -> connect succeeds
        myCoroutine::IOManager *iom = myCoroutine::IOManager::getThis();
        std::shared_ptr<myCoroutine::Timer> timer;
        std::shared_ptr<timer_info> tinfo(new timer_info);
        std::weak_ptr<timer_info> winfo(tinfo);

        if (timeout_ms != (uint64_t)-1)
        {
            timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]()
                                           {
            auto t = winfo.lock();
            if(!t || t->cancelled) 
            {
                return;
            }
            t->cancelled = ETIMEDOUT;
            iom->cancelEvent(fd, myCoroutine::IOManager::WRITE); }, winfo);
        }

        int rt = iom->addEvent(fd, myCoroutine::IOManager::WRITE);
        if (rt == 0)
        {
            myCoroutine::Fiber::getThis()->yield();

            // resume either by addEvent or cancelEvent
            if (timer)
            {
                timer->cancel();
            }

            if (tinfo->cancelled)
            {
                errno = tinfo->cancelled;
                return -1;
            }
        }
        else
        {
            if (timer)
            {
                timer->cancel();
            }
            std::cerr << "connect addEvent(" << fd << ", WRITE) error";
        }

        // check out if the connection socket established
        int error = 0;
        socklen_t len = sizeof(int);
        if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len))
        {
            return -1;
        }
        if (!error)
        {
            return 0;
        }
        else
        {
            errno = error;
            return -1;
        }
    }

    static uint64_t s_connect_timeout = -1;
    int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
    {
        return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
    }

    int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
    {
        int fd = do_io(sockfd, accept_f, "accept", myCoroutine::IOManager::READ, SO_RCVTIMEO, addr, addrlen);
        if (fd >= 0)
        {
            myCoroutine::FdMgr::GetInstance()->get(fd, true);
        }
        return fd;
    }

    ssize_t read(int fd, void *buf, size_t count)
    {
        return do_io(fd, read_f, "read", myCoroutine::IOManager::READ, SO_RCVTIMEO, buf, count);
    }

    ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
    {
        return do_io(fd, readv_f, "readv", myCoroutine::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);
    }

    ssize_t recv(int sockfd, void *buf, size_t len, int flags)
    {
        return do_io(sockfd, recv_f, "recv", myCoroutine::IOManager::READ, SO_RCVTIMEO, buf, len, flags);
    }

    ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    {
        return do_io(sockfd, recvfrom_f, "recvfrom", myCoroutine::IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
    }

    ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
    {
        return do_io(sockfd, recvmsg_f, "recvmsg", myCoroutine::IOManager::READ, SO_RCVTIMEO, msg, flags);
    }

    ssize_t write(int fd, const void *buf, size_t count)
    {
        return do_io(fd, write_f, "write", myCoroutine::IOManager::WRITE, SO_SNDTIMEO, buf, count);
    }

    ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
    {
        return do_io(fd, writev_f, "writev", myCoroutine::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);
    }

    ssize_t send(int sockfd, const void *buf, size_t len, int flags)
    {
        return do_io(sockfd, send_f, "send", myCoroutine::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags);
    }

    ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    {
        return do_io(sockfd, sendto_f, "sendto", myCoroutine::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);
    }

    ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
    {
        return do_io(sockfd, sendmsg_f, "sendmsg", myCoroutine::IOManager::WRITE, SO_SNDTIMEO, msg, flags);
    }

    int close(int fd)
    {
        if (!myCoroutine::t_hook_enable)
        {
            return close_f(fd);
        }

        std::shared_ptr<myCoroutine::FdCtx> ctx = myCoroutine::FdMgr::GetInstance()->get(fd);

        if (ctx)
        {
            auto iom = myCoroutine::IOManager::getThis();
            if (iom)
            {
                iom->cancelAll(fd);
            }
            // del fdctx
            myCoroutine::FdMgr::GetInstance()->del(fd);
        }
        return close_f(fd);
    }

    int fcntl(int fd, int cmd, ... /* arg */)
    {
        va_list va; // to access a list of mutable parameters

        va_start(va, cmd);
        switch (cmd)
        {
        case F_SETFL:
        {
            int arg = va_arg(va, int); // Access the next int argument
            va_end(va);
            std::shared_ptr<myCoroutine::FdCtx> ctx = myCoroutine::FdMgr::GetInstance()->get(fd);
            if (!ctx || ctx->isClosed() || !ctx->isSocket())
            {
                return fcntl_f(fd, cmd, arg);
            }
            // 更新用户设置的非阻塞状态
            ctx->setUserNonblock(arg & O_NONBLOCK);
            // 据系统阻塞状态返回arg
            if (ctx->getSysNonblock())
            {
                arg |= O_NONBLOCK;
            }
            else
            {
                arg &= ~O_NONBLOCK;
            }
            return fcntl_f(fd, cmd, arg);
        }
        break;

        case F_GETFL:
        {
            va_end(va);
            int arg = fcntl_f(fd, cmd);
            std::shared_ptr<myCoroutine::FdCtx> ctx = myCoroutine::FdMgr::GetInstance()->get(fd);
            // 若上下文不存在、已关闭或非socket，则返回状态标志
            if (!ctx || ctx->isClosed() || !ctx->isSocket())
            {
                return arg;
            }
            // 这里是呈现给用户 显示的为用户设定的值
            // 但是底层还是根据系统设置决定的
            if (ctx->getUserNonblock())
            {
                return arg | O_NONBLOCK;
            }
            else
            {
                return arg & ~O_NONBLOCK;
            }
        }
        break;

        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
        {
            int arg = va_arg(va, int);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        break;

        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
        {
            va_end(va);
            return fcntl_f(fd, cmd);
        }
        break;

        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
        {
            struct flock *arg = va_arg(va, struct flock *);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        break;

        case F_GETOWN_EX:
        case F_SETOWN_EX:
        {
            struct f_owner_exlock *arg = va_arg(va, struct f_owner_exlock *);
            va_end(va);
            return fcntl_f(fd, cmd, arg);
        }
        break;

        default:
            va_end(va);
            return fcntl_f(fd, cmd);
        }
    }

    /**
     * @brief ioctl系统调用的hook函数，用于拦截并处理特定的ioctl请求
     *        主要功能：对FIONBIO命令进行特殊处理（设置文件描述符的非阻塞模式），其他命令转发给原始ioctl函数
     *
     * @param fd 文件描述符，指向要操作的设备/文件
     * @param request ioctl控制命令（如FIONBIO等）
     * @param ... 可变参数，用于传递命令所需的输入/输出数据
     * @return int 调用结果：成功返回非负值，失败返回-1
     */
    int ioctl(int fd, unsigned long request, ...)
    {
        va_list va;                     // 声明变参指针，用于解析可变参数
        va_start(va, request);          // 初始化变参指针，从request后的第一个可变参数开始
        void *arg = va_arg(va, void *); // 获取可变参数中的数据指针（第三个参数）
        va_end(va);                     // 结束变参处理，释放相关资源

        // 处理"设置非阻塞I/O"命令（FIONBIO是POSIX定义的标准命令，用于设置文件描述符的非阻塞标志）
        if (FIONBIO == request)
        {
            // 将用户传入的参数解析为非阻塞状态：用户空间通常传入int*，0表示阻塞，非0表示非阻塞
            // !! 操作将任意非0值转为1（true），0保持为0（false）
            bool user_nonblock = !!*(int *)arg;

            std::shared_ptr<myCoroutine::FdCtx> ctx = myCoroutine::FdMgr::GetInstance()->get(fd);

            // 若上下文不存在、已关闭或非socket，则不处理，直接调用原始ioctl
            if (!ctx || ctx->isClosed() || !ctx->isSocket())
            {
                return ioctl_f(fd, request, arg);
            }

            // 若为有效的socket，更新其用户设置的非阻塞状态
            ctx->setUserNonblock(user_nonblock);
        }

        // 所有命令（包括已处理的FIONBIO）最终都转发给原始ioctl函数，保证系统调用语义完整
        return ioctl_f(fd, request, arg);
    }

    int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
    {
        return getsockopt_f(sockfd, level, optname, optval, optlen);
    }

    int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
    {
        if (!myCoroutine::t_hook_enable)
        {
            return setsockopt_f(sockfd, level, optname, optval, optlen);
        }

        if (level == SOL_SOCKET)
        {
            if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)
            {
                std::shared_ptr<myCoroutine::FdCtx> ctx = myCoroutine::FdMgr::GetInstance()->get(sockfd);
                if (ctx)
                {
                    const timeval *v = (const timeval *)optval;
                    ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
                }
            }
        }
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
}