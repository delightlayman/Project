#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cstring>

#include "ioscheduler.hpp"

static bool debug = true;

namespace myCoroutine
{

    // 获取当前调度器对象
    IOManager *IOManager::getThis()
    {
        // scheduler*是否可以安全的转换为IOManager*
        // 是：返回IOManager*，否：返回nullptr，抛出异常bad_cast
        return dynamic_cast<IOManager *>(Scheduler::getThis());
    }

    // 据事件类型，获取对应事件的上下文
    IOManager::FdContext::EventContext &IOManager::FdContext::getEventContext(Event event)
    {
        assert(event == READ || event == WRITE);
        switch (event)
        {
        case READ:
            return read;
        case WRITE:
            return write;
        }
        throw std::invalid_argument("Unsupported event type");
    }

    void IOManager::FdContext::resetEventContext(EventContext &ctx)
    {
        ctx.scheduler = nullptr;
        ctx.fiber.reset();
        ctx.cb = nullptr;
    }

    // 触发事件：将事件上下文中的回调函数、协程加入调度器任务队列
    // 事件出发后，该事件会从fdContext中删除
    void IOManager::FdContext::triggerEvent(Event event)
    {
        assert(events & event);

        // delete event
        events = (Event)(events & ~event);

        // trigger
        EventContext &ctx = getEventContext(event);
        if (ctx.cb)
        {
            // call ScheduleTask(std::function<void()>* f, int thr)
            ctx.scheduler->addscheduletask(&ctx.cb);
        }
        else
        {
            // call ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
            ctx.scheduler->addscheduletask(&ctx.fiber);
        }

        // reset event context
        resetEventContext(ctx);
        return;
    }

    IOManager::IOManager(size_t threads, bool use_caller, const std::string &name) : Scheduler(threads, use_caller, name), TimerManager()
    {
        // create epoll fd
        _epfd = epoll_create(5000);
        assert(_epfd > 0);

        // create pipe
        int rt = pipe(_tickleFds);
        assert(!rt);

        // add read event to epoll
        epoll_event event;
        event.events = EPOLLIN | EPOLLET; // Edge Triggered
        event.data.fd = _tickleFds[0];

        // non-blocked
        rt = fcntl(_tickleFds[0], F_SETFL, O_NONBLOCK);
        assert(!rt);
        // make _tickleFds[0] focus on event--->EPOLLIN | EPOLLET
        rt = epoll_ctl(_epfd, EPOLL_CTL_ADD, _tickleFds[0], &event);
        assert(!rt);

        contextResize(32);

        start();
    }

    IOManager::~IOManager()
    {
        stop();
        close(_epfd);
        close(_tickleFds[0]);
        close(_tickleFds[1]);

        for (size_t i = 0; i < _fdContexts.size(); ++i)
        {
            if (_fdContexts[i])
            {
                delete _fdContexts[i];
            }
        }
    }

    // no lock
    void IOManager::contextResize(size_t size)
    {
        _fdContexts.resize(size);

        for (size_t i = 0; i < _fdContexts.size(); ++i)
        {
            // 初始化新增的fdContext
            if (_fdContexts[i] == nullptr)
            {
                _fdContexts[i] = new FdContext();
                _fdContexts[i]->fd = i;
            }
        }
    }

    int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
    {
        // attemp to find FdContext
        FdContext *fd_ctx = nullptr;

        std::shared_lock<std::shared_mutex> read_lock(_mutex); // 读锁
        if ((int)_fdContexts.size() > fd)
        {
            fd_ctx = _fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            std::unique_lock<std::shared_mutex> write_lock(_mutex); // 写锁
            contextResize(fd * 1.5);                                // 按fd*1.5扩容，不是按原大小*1.5扩容
            fd_ctx = _fdContexts[fd];
        }

        std::lock_guard<std::mutex> lock(fd_ctx->_mutex);

        // the event has already been added
        if (fd_ctx->events & event)
        {
            return -1;
        }

        // add new event
        int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        epoll_event epevent;
        epevent.events = EPOLLET | fd_ctx->events | event;
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(_epfd, op, fd, &epevent);
        if (rt)
        {
            std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        ++_pendingEventCount;

        // update fdcontext
        fd_ctx->events = (Event)(fd_ctx->events | event);

        // update event context
        FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
        // event对应的事件上下文应该是未使用状态
        assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);
        // 事件上下文的调度器设置为当前线程的调度器
        event_ctx.scheduler = Scheduler::getThis();
        if (cb)
        {
            event_ctx.cb.swap(cb);
        }
        else
        {
            // 事件上下文的协程设置为当前线程中正在运行的协程
            event_ctx.fiber = Fiber::getThis();
            assert(event_ctx.fiber->getstate() == Fiber::Running);
        }
        return 0;
    }

    bool IOManager::delEvent(int fd, Event event)
    {
        // attemp to find FdContext
        FdContext *fd_ctx = nullptr;

        std::shared_lock<std::shared_mutex> read_lock(_mutex);
        if ((int)_fdContexts.size() > fd)
        {
            fd_ctx = _fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> lock(fd_ctx->_mutex);

        // the event doesn't exist
        if (!(fd_ctx->events & event))
        {
            return false;
        }

        // delete the event
        Event new_events = (Event)(fd_ctx->events & ~event);
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(_epfd, op, fd, &epevent);
        if (rt)
        {
            std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        --_pendingEventCount;

        // update fdcontext
        fd_ctx->events = new_events;

        // update event context
        FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
        fd_ctx->resetEventContext(event_ctx);
        return true;
    }

    bool IOManager::cancelEvent(int fd, Event event)
    {
        // attemp to find FdContext
        FdContext *fd_ctx = nullptr;

        std::shared_lock<std::shared_mutex> read_lock(_mutex);
        if ((int)_fdContexts.size() > fd)
        {
            fd_ctx = _fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> lock(fd_ctx->_mutex);

        // the event doesn't exist
        if (!(fd_ctx->events & event))
        {
            return false;
        }

        // delete the event
        Event new_events = (Event)(fd_ctx->events & ~event);
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(_epfd, op, fd, &epevent);
        if (rt)
        {
            std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        --_pendingEventCount;

        // update fdcontext, event context and trigger
        fd_ctx->triggerEvent(event);
        return true;
    }

    bool IOManager::cancelAll(int fd)
    {
        // attemp to find FdContext
        FdContext *fd_ctx = nullptr;

        std::shared_lock<std::shared_mutex> read_lock(_mutex);
        if ((int)_fdContexts.size() > fd)
        {
            fd_ctx = _fdContexts[fd];
            read_lock.unlock();
        }
        else
        {
            read_lock.unlock();
            return false;
        }

        std::lock_guard<std::mutex> lock(fd_ctx->_mutex);

        // none of events exist
        if (!fd_ctx->events)
        {
            return false;
        }

        // delete all events
        int op = EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = 0;
        epevent.data.ptr = fd_ctx;

        int rt = epoll_ctl(_epfd, op, fd, &epevent);
        if (rt)
        {
            std::cerr << "IOManager::epoll_ctl failed: " << strerror(errno) << std::endl;
            return -1;
        }

        // update fdcontext, event context and trigger
        if (fd_ctx->events & READ)
        {
            fd_ctx->triggerEvent(READ);
            --_pendingEventCount;
        }

        if (fd_ctx->events & WRITE)
        {
            fd_ctx->triggerEvent(WRITE);
            --_pendingEventCount;
        }

        assert(fd_ctx->events == 0);
        return true;
    }

    // 唤醒线程
    // 检测有空闲线程时，向管道写一个字节，唤醒一个等待任务的线程
    void IOManager::tickle()
    {
        // no idle threads
        if (!hasIdleThreads())
        {
            return;
        }
        // 管道：写端写入一个字符，使得阻塞的读端被唤醒
        int rt = write(_tickleFds[1], "T", 1);
        assert(rt == 1);
    }
    // 判断是否可停止---无定时器 && 无待处理事件 && 调度器已停止
    bool IOManager::stopping()
    {
        uint64_t timeout = getNextTimer();
        // no timers left and no pending events left with the Scheduler::stopping()
        return timeout == ~0ull && _pendingEventCount == 0 && Scheduler::stopping();
    }
    // 处理就绪事件和超时定时器
    void IOManager::idle()
    {
        static const uint64_t MAX_EVENTS = 256;
        std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVENTS]);

        while (true)
        {
            if (debug)
                std::cout << "IOManager::idle(),run in thread: " << Thread::getSysTid() << std::endl;

            if (stopping())
            {
                if (debug)
                    std::cout << "name = " << getname() << " idle exits in thread: " << Thread::getSysTid() << std::endl;
                break;
            }

            // blocked at epoll_wait
            int rt = 0; // 就绪事件个数
            while (true)
            {
                static const uint64_t MAX_TIMEOUT = 5000; // 5000ms
                uint64_t next_timeout = getNextTimer();   // 获取最近的超时时间
                next_timeout = std::min(next_timeout, MAX_TIMEOUT);

                rt = epoll_wait(_epfd, events.get(), MAX_EVENTS, (int)next_timeout);
                // EINTR -> retry
                if (rt < 0 && errno == EINTR) // 被信号打断导致epoll_wait失败
                {
                    continue;
                }
                else
                {
                    break;
                }
            };

            // collect all timers overdue
            std::vector<std::function<void()>> cbs;
            listExpiredCb(cbs); // 取出所有超时定时器的回调函数，存放到cbs中
            if (!cbs.empty())
            {
                for (const auto &cb : cbs)
                {
                    addscheduletask(cb);
                }
                cbs.clear();
            }

            // collect all events ready
            for (int i = 0; i < rt; ++i)
            {
                // unique_ptr 针对数组类型，有operator[]重载
                // 获取就绪事件
                epoll_event &event = events[i];

                // tickle event
                if (event.data.fd == _tickleFds[0])
                {
                    uint8_t dummy[256];
                    // edge triggered -> exhaust
                    while (read(_tickleFds[0], dummy, sizeof(dummy)) > 0);
                    continue;
                }

                // other events
                FdContext *fd_ctx = (FdContext *)event.data.ptr;
                std::lock_guard<std::mutex> lock(fd_ctx->_mutex);

                // convert EPOLLERR or EPOLLHUP to -> read or write event
                // 错误或挂起事件 转换为 可读或可写事件
                if (event.events & (EPOLLERR | EPOLLHUP))
                {
                    event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
                }
                // events happening during this turn of epoll_wait
                int real_events = NONE;
                if (event.events & EPOLLIN)
                {
                    real_events |= READ;
                }
                if (event.events & EPOLLOUT)
                {
                    real_events |= WRITE;
                }
                // 关注事件与实际发生的事件是否匹配
                if ((fd_ctx->events & real_events) == NONE)
                {
                    continue;
                }

                // delete the events that have already happened
                // if the events left are not NONE modify fd , or delete fd
                int left_events = (fd_ctx->events & ~real_events);
                int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
                event.events = EPOLLET | left_events;

                int rt2 = epoll_ctl(_epfd, op, fd_ctx->fd, &event);
                if (rt2)
                {
                    std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl;
                    continue;
                }

                // schedule callback and update fdcontext and event context
                if (real_events & READ)
                {
                    fd_ctx->triggerEvent(READ);
                    --_pendingEventCount;
                }
                if (real_events & WRITE)
                {
                    fd_ctx->triggerEvent(WRITE);
                    --_pendingEventCount;
                }
            } // end for

            // 一旦处理完所有的事件，idle协程yield，这样可以让调度协程(Scheduler::run)重新检查是否有新任务要调度
            // 上面triggerEvent实际也只是把对应的fiber重新加入调度，要执行的话还要等idle协程退出
            // 注意：由于运行协程和调度协程的切换，mainfunc中的cur释放问题，计数是否需要-1
            Fiber::getThis()->yield();

        } // end while(true)
    }

    void IOManager::onTimerInsertedAtFront()
    {
        tickle();
    }

} // end namespace sylar