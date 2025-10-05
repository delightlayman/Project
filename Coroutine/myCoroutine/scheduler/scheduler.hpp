// 调度器内部维护一个任务队列和一个调度线程池

#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include "../fiber/fiber.hpp"
#include "../thread/thread.hpp"

#include <mutex>
#include <vector>
#include <list>

using std::bind;
using std::cerr;
using std::function;
using std::list;
using std::lock_guard;
using std::make_shared;
using std::mutex;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::vector;

namespace myCoroutine
{

    class Scheduler
    {
    public:
        Scheduler(size_t threads = 1, bool usecaller = true, const string &name = "Scheduler");
        virtual ~Scheduler();

        const string &getname() const { return _name; }

    public:
        static Scheduler *getThis(); // 获取正在运行的调度器

    protected:
        void setThis(); // 设置正在运行的调度器

    public:
        // 添加任务到任务队列
        template <class Fiber_Or_Cb>
        void addscheduletask(Fiber_Or_Cb fc, int thread = -1)
        {
            bool need_tickle = false;
            {
                lock_guard<mutex> lock(_mutex);
                // empty ->  all thread is idle -> need to be waken up
                need_tickle = _tasks.empty();

                ScheduleTask task(fc, thread);
                if (task.fiber || task.cb)
                {
                    _tasks.push_back(task);
                }
            }

            if (need_tickle)
            {
                tickle();
            }
        }

        virtual void start(); // 启动调度器-->启动线程池，
        virtual void stop();  // 关闭调度器-->关闭线程池，---所有调度任务执行后返回

    protected:
        virtual void tickle();   // 唤醒线程---当前无实际作用，后续重载
        virtual void run();      // 线程函数---回调函数
        virtual void idle();     // 空闲协程函数
        virtual bool stopping(); // 是否已关闭

        // 是否有空闲线程
        bool hasIdleThreads() { return _idleThreadCount > 0; }

    private:
        // 任务---可以是协程，也可以是回调函数
        struct ScheduleTask
        {
            shared_ptr<Fiber> fiber;
            function<void()> cb;
            int thread; // 指定任务需要运行的线程id

            ScheduleTask()
            {
                fiber = nullptr;
                cb = nullptr;
                thread = -1;
            }

            ScheduleTask(shared_ptr<Fiber> f, int thr)
            {
                fiber = f;
                thread = thr;
            }

            ScheduleTask(shared_ptr<Fiber> *f, int thr)
            {
                fiber.swap(*f); // 数据交换，不改变计数
                thread = thr;
            }

            ScheduleTask(function<void()> f, int thr)
            {
                cb = f;
                thread = thr;
            }

            ScheduleTask(function<void()> *f, int thr)
            {
                cb.swap(*f);
                thread = thr;
            }
            // shared_ptr<A> a = make_shared<A>();
            //赋值场景	     调用函数	                    语义	                引用计数变化
            // a = b;	    赋值运算符（const shared_ptr&）	共享 b 持有的资源所有权  引用计数加 1
            // a = nullptr;	空指针赋值运算符（nullptr_t）	放弃 a 持有的资源所有权	  减 1（若有资源）
            void reset()
            {
                fiber = nullptr;
                cb = nullptr;
                thread = -1;
            }
        };

    private:
        string _name;                            // 调度器名称
        mutex _mutex;                            // 互斥锁 -> 保护任务队列
        vector<shared_ptr<Thread>> _threads;     // 线程池
        list<ScheduleTask> _tasks;               // 任务队列
        vector<int> _threadIds;                  // 存储工作线程的线程id
        size_t _threadCount = 0;                 // 需要创建的线程数
        atomic<size_t> _activeThreadCount = {0}; // 活跃线程数
        atomic<size_t> _idleThreadCount = {0};   // 空闲线程数

        // 主线程是否使用调度协程
        // 是则：主线程添加任务，调度协程管理各从线程，各从线程安排协程完成任务
        // 否则：主线程只负责添加任务，各从线程各自安排协程完成任务
        bool _usecaller;
        shared_ptr<Fiber> _schedulerFiber = nullptr; // 需创建的调度协程，
        int _rootThread = -1;                        // 记录主线程的线程id

        bool _stopping = false; // 是否正在关闭（其他资源可能未处理）
    };

}

#endif