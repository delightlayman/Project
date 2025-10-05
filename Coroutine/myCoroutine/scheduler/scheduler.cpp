#include "scheduler.hpp"

static bool debug = true;

namespace myCoroutine
{
    // 当前线程的调度器
    static thread_local Scheduler *t_scheduler = nullptr;

    // 获取当前线程的调度器
    Scheduler *Scheduler::getThis()
    {
        return t_scheduler;
    }
    // 设置当前线程的调度器
    void Scheduler::setThis()
    {
        t_scheduler = this;
    }

    Scheduler::Scheduler(size_t threads, bool usecaller, const string &name) : _usecaller(usecaller), _name(name)
    {
        // 线程池大小>0 且 当前线程没有调度器
        assert(threads > 0 && Scheduler::getThis() == nullptr);
        setThis();
        // 主线程名称 同 调度器名称
        Thread::setName(_name);

        // 使用主线程参与调度
        if (_usecaller)
        {
            threads--;

            // 创建主协程
            Fiber::getThis();

            // 创建调度协程--- 无调用栈 且 调度协程退出后将返回主协程
            // 注意：绑定了this---调度器对象
            // 主线程回调函数交给协程运行
            _schedulerFiber.reset(new Fiber(bind(&Scheduler::run, this), Fiber::STACKSIZE, false));
            Fiber::setSchedulerFiber(_schedulerFiber.get());
            _rootThread = Thread::getSysTid();

            _threadIds.push_back(_rootThread);
        }

        _threadCount = threads;
        if (debug)
            cout << "Scheduler constructor success\n";
    }

    Scheduler::~Scheduler()
    {
        // 析构器中，调度器应处于停止状态
        assert(stopping() == true);
        // 若析构当前线程的调度器，则将其置空
        if (getThis() == this)
        {
            t_scheduler = nullptr;
        }
        if (debug)
            cout << "Scheduler destructor success\n";
    }
    // 启动调度器
    void Scheduler::start()
    {
        lock_guard<mutex> lock(_mutex);
        // 调度器若停止，则退出
        if (_stopping)
        {
            cerr << "Scheduler is stopped" << endl;
            return;
        }
        // 线程池启动前，线程池应该为空
        assert(_threads.empty());
        _threads.resize(_threadCount);
        for (size_t i = 0; i < _threadCount; i++)
        {
            // 注意：绑定了this---调度器对象
            _threads[i].reset(new Thread(bind(&Scheduler::run, this), _name + "_" + to_string(i)));
            _threadIds.push_back(_threads[i]->getstid());
        }
        if (debug)
            cout << "Scheduler::start() success\n";
    }

    void Scheduler::run()
    {
        // 当前线程内核ID
        int cur_stid = Thread::getSysTid();
        if (debug)
            cout << "Schedule::run() starts in thread: " << cur_stid << endl;

        // 当前线程的调度器
        // 由于绑定了来自主线程调度器的this，故实际上调度器就一个，即主线程调度器
        setThis();

        // 当前线程为新建线程，非主线程 -> 需要创建主协程（此时：主协程即调度携程）
        if (cur_stid != _rootThread)
        {
            Fiber::getThis();
        }
        // 创建空闲协程---若没有任务，则一直执行空闲协程，浪费资源
        shared_ptr<Fiber> idle_fiber = make_shared<Fiber>(bind(&Scheduler::idle, this));
        ScheduleTask task;

        while (true)
        {
            task.reset();
            bool tickle_me = false; // 是否tickle其他线程进行任务调度

            {
                lock_guard<mutex> lock(_mutex);
                auto it = _tasks.begin();
                // 遍历任务队列---寻找任务的指定执行线程执行任务
                // 若任务指定线程非当前线程，则唤醒/通知（tickle）其他线程进行任务调度
                // 若任务未处理完，则唤醒/通知（tickle）其他线程进行任务调度
                // 若任务指定线程为当前线程，则取出任务并执行
                // 1 遍历任务队列
                while (it != _tasks.end())
                {
                    // 指定线程非空 且 指定线程不是当前线程---标记一下需要通知
                    if (it->thread != -1 && it->thread != cur_stid)
                    {
                        it++;
                        tickle_me = true;
                        continue;
                    }
                    // 找到一个未指定线程，或是指定了当前线程的任务
                    //  2 取出任务
                    assert(it->fiber || it->cb);
                    task = *it;
                    _tasks.erase(it);
                    _activeThreadCount++;
                    break;
                }
                tickle_me = tickle_me || (it != _tasks.end());
            }

            if (tickle_me)
            {
                tickle();
            }

            // 3 执行任务
            if (task.fiber)
            {
                {
                    // 协程执行时，用协程自带锁锁定，防止其他线程访问此协程修改其数据
                    lock_guard<mutex> lock(task.fiber->_mutex);
                    if (task.fiber->getstate() != Fiber::Term)
                    {
                        task.fiber->resume();
                        cout << "----1" << endl;
                    }
                }
                cout << "----2" << endl;
                _activeThreadCount--;
                task.reset();
                cout << "----3" << endl;
            }
            else if (task.cb)
            {
                shared_ptr<Fiber> cb_fiber = make_shared<Fiber>(task.cb);
                {
                    lock_guard<mutex> lock(cb_fiber->_mutex);
                    cb_fiber->resume();
                }
                _activeThreadCount--;
                task.reset();
            }
            // 4 无任务 -> 执行空闲协程
            else
            {
                // 系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出循环并退出run()
                if (idle_fiber->getstate() == Fiber::Term)
                {
                    if (debug)
                        cout << "Schedule::run() ends in thread: " << cur_stid << endl;
                    break;
                }
                _idleThreadCount++;//空闲线程数+1
                idle_fiber->resume();
                _idleThreadCount--;//空闲线程数-1
            }
        }
    }
    // 关闭调度器
    void Scheduler::stop()
    {
        if (debug)
            cout << "Schedule::stop() starts in thread: " << Thread::getSysTid() << endl;

        // 调度器已关闭，则退出
        if (stopping())
        {
            return;
        }

        _stopping = true;

        if (_usecaller)
        {
            assert(getThis() == this);
        }
        else
        {
            assert(getThis() != this);
        }

        for (size_t i = 0; i < _threadCount; i++)
        {
            tickle();
        }

        if (_schedulerFiber)
        {
            tickle();
        }

        if (_schedulerFiber)
        {
            // 启动调度协程处理任务
            _schedulerFiber->resume();
            if (debug)
                cout << "_schedulerFiber ends in thread:" << Thread::getSysTid() << endl;
        }

        vector<shared_ptr<Thread>> thrs;
        {
            lock_guard<mutex> lock(_mutex);
            thrs.swap(_threads);
        }

        for (auto &i : thrs)
        {
            i->join();
        }
        if (debug)
            cout << "Schedule::stop() ends in thread:" << Thread::getSysTid() << endl;
    }
    // 目前无实际作用，后续重载---唤醒线程
    void Scheduler::tickle()
    {
        cout << "tickle() " << endl;
    }

    void Scheduler::idle()
    {
        while (!stopping())
        {
            if (debug)
                cout << "Scheduler::idle(), sleeping in thread: " << Thread::getSysTid() << endl;
            sleep(1);
            Fiber::getThis()->yield();
        }
    }

    // 调度器是否已经关闭
    bool Scheduler::stopping()
    {
        lock_guard<mutex> lock(_mutex);
        return _stopping && _tasks.empty() && _activeThreadCount == 0;
    }

}