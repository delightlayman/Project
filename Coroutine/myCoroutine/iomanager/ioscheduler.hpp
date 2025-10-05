#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__

#include "../scheduler/scheduler.hpp"
#include "../timer/timer.hpp"

namespace myCoroutine
{

    // work flow
    // 1 register one event -> 2 wait for it to ready -> 3 schedule the callback -> 4 unregister the event -> 5 run the callback
    // 1 注册一个事件 -> 2 等待事件就绪 -> 3 调度回调 -> 4 注销事件 -> 5 运行回调
    class IOManager : public Scheduler, public TimerManager
    {
    public:
        // IO事件---三大类：读、写、无
        enum Event
        {
            NONE = 0x0,
            // READ == EPOLLIN
            READ = 0x1,
            // WRITE == EPOLLOUT
            WRITE = 0x4
        };

    private:
        struct FdContext
        {
            struct EventContext
            {
                // scheduler
                Scheduler *scheduler = nullptr;
                // callback fiber
                shared_ptr<Fiber> fiber;
                // callback function
                function<void()> cb;
            };
            // file descriptor 
            int fd = 0;
            // read event context
            EventContext read;
            // write event context
            EventContext write;
            // events registered
            Event events = NONE;
            // event mutex
            mutex _mutex;

            EventContext &getEventContext(Event event);
            void resetEventContext(EventContext &ctx);
            void triggerEvent(Event event);
        };

    public:
        IOManager(size_t threads = 1, bool use_caller = true, const string &name = "IOManager");
        ~IOManager();

        // add one event at a time
        int addEvent(int fd, Event event, function<void()> cb = nullptr);
        // delete event
        bool delEvent(int fd, Event event);
        // delete the event and trigger its callback
        bool cancelEvent(int fd, Event event);
        // delete all events and trigger its callback
        bool cancelAll(int fd);
        // get current IOManager
        static IOManager *getThis();

    protected:
        // notify the IOManager that there are new tasks to be scheduled
        void tickle() override;
        // whether the IOManager could be stopped
        bool stopping() override;
        // collect all triggered fd callback and set to IOManager
        void idle() override;
        // when timer insert at front, update epoll_wait time and wake up idle coroutine
        void onTimerInsertedAtFront() override;
        // resize the IOManager fdContexts vector
        void contextResize(size_t size);

    private:
        // epoll fd
        int _epfd = 0;
        // pipe fd[0] read，fd[1] write
        int _tickleFds[2];
        // counts for events pended to be processed
        atomic<size_t> _pendingEventCount = {0};
        // IOManager lock
        shared_mutex _mutex;
        // store fdcontexts for each fd---fdContext pool
        // fd == index
        vector<FdContext *> _fdContexts;
    };

} // end namespace sylar

#endif