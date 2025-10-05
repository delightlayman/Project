#ifndef _TIMER_H_
#define _TIMER_H_

#include <memory>
#include <vector>
#include <set>
#include <shared_mutex>
#include <assert.h>
#include <functional>
#include <mutex>
#include <chrono>

using std::function;
using std::mutex;
using std::set;
using std::shared_lock;
using std::shared_mutex;
using std::shared_ptr;
using std::unique_lock;
using std::vector;
using std::weak_ptr;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::system_clock;
using std::chrono::time_point;

namespace myCoroutine
{
    class TimerManager;

    class Timer : public std::enable_shared_from_this<Timer>
    {
    public:
        using func = function<void()>;
        // 友元类
        friend class TimerManager;

    public:
        // 取消timer
        bool cancel();
        // 刷新timer
        bool refresh();
        // 重设timer
        bool reset(uint64_t ms, bool from_now);

    private:
        Timer(uint64_t ms, func cb, bool recurring, TimerManager *manager);

    private:
        // 是否循环使用
        bool _recurring = false;
        // 超时时间间隔
        uint64_t _ms = 0;
        // 绝对超时时间
        time_point<system_clock> _next;
        // 超时时触发的回调函数
        func _cb;
        // 管理此timer的管理器
        TimerManager *_manager = nullptr;

    private:
        // 实现最小堆的比较函数
        struct Comparator
        {
            bool operator()(const shared_ptr<Timer> &lhs, const shared_ptr<Timer> &rhs) const;
        };
    };

    class TimerManager
    {
    public:
        using func = function<void()>;
        // 友元类
        friend class Timer;

    public:
        TimerManager();
        virtual ~TimerManager();
        // 添加timer
        shared_ptr<Timer> addTimer(uint64_t ms, func cb, bool recurring = false);
        // 添加条件timer
        shared_ptr<Timer> addConditionTimer(uint64_t ms, func cb, weak_ptr<void> weak_cond, bool recurring = false);

        // 拿到堆中最近的超时时间间隔
        uint64_t getNextTimer();
        // 取出所有超时定时器的回调函数，存放到cbs中
        void listExpiredCb(vector<func> &cbs);
        // 堆中是否有timer
        bool hasTimer();

    protected:
        // 新的定时器插入到定时器的首部 ->执行该函数
        // 目前无实际作用,后续重载
        // 通知外部有新的 “最早到期” 定时器插入，用于唤醒等待中的事件循环
        virtual void onTimerInsertedAtFront() {};

        // 添加timer
        void addTimer(shared_ptr<Timer> timer);

    private:
        // 服务器系统时间改变 -> 调用该函数
        // 检测系统时钟是否发生 “回退”（如手动修改系统时间到过去，硬件时钟故障等等），避免定时器因时钟异常导致逻辑错误。
        bool detectClockRollover();

    private:
        // 读写锁
        shared_mutex _mutex;
        // 时间堆---集合实现
        set<shared_ptr<Timer>, Timer::Comparator> _timers;
        // 在下次getNextTime()执行前 onTimerInsertedAtFront()是否已经被触发了 -> 在此过程中 onTimerInsertedAtFront()只执行一次
        bool _tickled = false;
        // 上次检查系统时间是否回退的绝对时间
        time_point<system_clock> _previouseTime;
    };

}

#endif