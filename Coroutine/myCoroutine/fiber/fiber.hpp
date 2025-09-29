// 非对称，有栈（独立栈）协程，且无嵌套
#ifndef _FIBER_H_
#define _FIBER_H_

#include <iostream>
#include <memory>
#include <ucontext.h>
#include <mutex>
#include <functional>
#include <atomic>
#include <pthread.h>
#include <cassert>

using std::atomic;
using std::cout, std::endl;
using std::function;
using std::mutex;
using std::shared_ptr;

namespace myCoroutine
{
    /*
    1. Fiber对象均使用shared_ptr进行管理
    2. std::enable_shared_from_this<Fiber> 的核心作用是：
        让 Fiber 类的对象在被 shared_ptr 管理时，能安全地获取指向自身的 shared_ptr，确保引用计数正确，避免内存错误。
        适用将对象自身作为 shared_ptr 传递的场景（如回调、事件注册）
    */
    class Fiber : public std::enable_shared_from_this<Fiber>
    {
    public:
        // 协程状态
        enum state
        {
            Ready,
            Running,
            Term
        };
        // 入口函数类型
        using func = function<void()>;

    private:
        // 构造主协程---私有，仅由getThis()调用
        Fiber();

    public:
        static constexpr uint32_t STACKSIZE = 4096 * 8;
        // 构造子协程
        Fiber(const func &cb, const uint32_t stacksize = STACKSIZE, bool inscheduler = true);
        ~Fiber();

        void reset(const func &cb); // 重用一个协程
        void resume();              // 任务协程恢复执行
        void yield();               // 任务协程终止执行

        uint64_t getid() const { return _id; }
/**
 * 获取当前状态的函数
 * @return 返回当前的状态值
 */
        state getstate() const { return _state; }

    public:
        // 静态方法，用以快速访问当前线程中的协程信息
        static shared_ptr<Fiber> getThis();      // 得到当前运行的协程，若无运行协程，则无主协程，故创建主协程并运行
        static void setThis(Fiber *f);           // 设置当前运行的协程
        static void setSchedulerFiber(Fiber *f); // 设置调度协程（默认为主协程）
        static uint64_t getFiberId();            // 得到当前运行的协程id
        static void mainFunc();                  // 协程入口函数

    private:
        uint64_t _id = 0;          // 协程id
        state _state = Ready;      // 协程状态
        uint32_t _stacksize = 0;   // 栈大小
        void *_stack = nullptr;    // 栈地址
        ucontext_t _ctx;           // 上下文
        func _cb;                  // 回调函数
        bool _inscheduler = false; // 是否由调度器调度
    public:
        mutex _mutex;
    };

}

#endif