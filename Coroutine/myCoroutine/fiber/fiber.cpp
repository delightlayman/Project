#include "fiber.hpp"

static bool debug = true; // 是否打印调试信息

namespace myCoroutine
{
    //同一线程，同一时刻，有且只有一个协程在运行（占有cpu）
    // 当前线程的协程信息
    static thread_local shared_ptr<Fiber> t_thread_fiber = nullptr; // 主协程
    static thread_local Fiber *t_scheduler_fiber = nullptr;         // 调度协程
    static thread_local Fiber *t_fiber = nullptr;                   // 运行中的协程

    // 全局协程计数器
    static atomic<uint64_t> s_fiber_id{0};
    // 活跃协程计数器
    static atomic<uint64_t> s_fiber_count{0};

    // 获取---当前线程---运行中的协程 若无运行协程，则无主协程，故创建主协程并运行
    shared_ptr<Fiber> Fiber::getThis()
    {
        if (t_fiber == nullptr)
        {
            shared_ptr<Fiber> main_fiber(new Fiber());
            t_thread_fiber = main_fiber;          // 主协程
            t_scheduler_fiber = main_fiber.get(); // 主线程默认为调度线程
            t_fiber = main_fiber.get();           // 主协程设置为运行中协程
        }

        return t_fiber->shared_from_this();
    }
    // 设置---当前线程---运行中的协程
    void Fiber::setThis(Fiber *f)
    {
        t_fiber = f;
    }
    // 设置---当前线程---调度协程
    void Fiber::setSchedulerFiber(Fiber *f)
    {
        t_scheduler_fiber = f;
    }
    // 得到---当前线程---运行中的协程的id
    uint64_t Fiber::getFiberId()
    {
        if (t_fiber)
            return t_fiber->getid();
        return (uint64_t)-1; //-1表示当前线程没有运行中的协程
    }
    // 创建主协程---负责调度，无需调用栈+回调函数+调度器调度
    Fiber::Fiber()
    {
        // 保存上下文---失败则退出当前线程
        if (getcontext(&_ctx) == -1)
        {
            perror("default constructor getcontext failed");
            pthread_exit(nullptr);
        }

        // 状态设置---主协程初始状态即为运行
        _state = Running;

        // id分配与活跃线程计数
        _id = s_fiber_id++;
        s_fiber_count++;
        if (debug)
            cout << "create main Fiber id=" << _id << endl;
    }

    // 创建子协程
    Fiber::Fiber(const func &cb, const uint32_t stacksize, bool inscheduler)
        : _cb(cb), _stacksize(stacksize), _inscheduler(inscheduler)
    {
        // 保存上下文---失败则退出当前线程
        if (getcontext(&_ctx) == -1)
        {
            perror("constructor getcontext failed");
            pthread_exit(nullptr);
        }
        // 下一个上下文---此处不设置，yield中设置
        _ctx.uc_link = nullptr;
        // 分配栈空间
        _stack = malloc(_stacksize);
        _ctx.uc_stack.ss_sp = _stack;
        _ctx.uc_stack.ss_size = _stacksize;
        // 设置回调函数
        makecontext(&_ctx, &Fiber::mainFunc, 0);
        // 设置状态
        _state = Ready;
        // id分配与活跃线程计数
        _id = s_fiber_id++;
        s_fiber_count++;
        if (debug)
            cout << "create son Fiber id=" << _id << endl;
    }
    Fiber::~Fiber()
    {
        s_fiber_count--;
        if (_stack)
            free(_stack);
        if (debug)
            cout << "destructor id=" << _id << endl;
    }

    // 子协程入口函数
    void Fiber::mainFunc()
    {
        // 获取运行中协程
        shared_ptr<Fiber> cur = getThis();
        assert(cur != nullptr);
        // 调用回调函数
        cur->_cb();
        // 回调函数执行完毕，置空回调函数+设置状态为终止
        cur->_cb = nullptr;
        cur->_state = Term;

        // 终止当前协程，执行权交回给调度器或主协程
        // yield切换上下文后，后续函数无法执行，故须先释放cur（引用计数-1+置空）
        // 注意：引用计数-1，局部指针cur置空，原指针仍有效
        auto raw_ptr = cur.get();
        cur.reset(); // 引用计数-1，并置空cur
        raw_ptr->yield();
    }
    // 重置协程---Term状态子协程 状态重置Ready，重置回调函数，重置上下文，复用栈空间
    void Fiber::reset(const func &cb)
    {
        // 终止的子协程才能重置
        assert(_state == Term && _stack != nullptr);
        if (getcontext(&_ctx) == -1)
        {
            perror("reset failed");
            pthread_exit(nullptr);
        }
        _ctx.uc_link = nullptr;
        _ctx.uc_stack.ss_sp = _stack;
        _ctx.uc_stack.ss_size = _stacksize;
        makecontext(&_ctx, &Fiber::mainFunc, 0);

        _cb = cb;
        _state = Ready;
    }
    // 恢复协程执行
    void Fiber::resume()
    {
        assert(_state == Ready);
        // 设置当前线程运行中的协程为当前协程
        setThis(this);
        _state = Running;
        if (_inscheduler) // 调度器调度
        {
            // 保存当前上下文到调度协程，恢复到子协程的上下文
            if (swapcontext(&t_scheduler_fiber->_ctx, &_ctx) == -1)
            {
                perror("resume t_scheduler_fiber to _ctx failed");
                pthread_exit(nullptr);
            }
        }
        else // 主协程调度
        {
            // 保存当前上下文到主协程，恢复到子协程的上下文
            if (swapcontext(&t_thread_fiber->_ctx, &_ctx) == -1)
            {
                perror("resume t_thread_fiber to _ctx failed");
                pthread_exit(nullptr);
            }
        }
    }
    // 终止协程执行
    void Fiber::yield()
    {
        assert(_state == Running || _state == Term);
        if (_state == Running) // 空闲协程未执行有效任务故而恢复为Ready状态
            _state = Ready;

        if (_inscheduler) // 调度器调度
        {
            setThis(t_scheduler_fiber);
            // 保存当前上下文到子协程，恢复到调度协程的上下文
            if (swapcontext(&_ctx, &t_scheduler_fiber->_ctx) == -1)
            {
                perror("resume _ctx to  t_scheduler_fiber failed");
                pthread_exit(nullptr);
            }
        }
        else // 主协程调度
        {
            setThis(t_thread_fiber.get());
            // 保存当前上下文到子协程，恢复到主协程的上下文
            if (swapcontext(&_ctx, &t_thread_fiber->_ctx) == -1)
            {
                perror("resume _ctx to t_scheduler_fiber failed");
            }
        }
    }
}