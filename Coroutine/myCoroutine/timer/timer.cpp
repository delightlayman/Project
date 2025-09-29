#include "timer.hpp"

namespace myCoroutine
{
    bool Timer::cancel()
    {
        unique_lock<shared_mutex> write_lock(_manager->_mutex);

        // 回调函数为空，说明此定时器已被处理了
        if (_cb == nullptr)
        {
            return false;
        }
        _cb = nullptr;

        auto it = _manager->_timers.find(shared_from_this());
        if (it != _manager->_timers.end())
        {
            _manager->_timers.erase(it);
        }
        return true;
    }

    // 刷新定时器---执行时间不变，刷新终止时间
    bool Timer::refresh()
    {
        unique_lock<shared_mutex> write_lock(_manager->_mutex);

        if (!_cb)
        {
            return false;
        }

        auto it = _manager->_timers.find(shared_from_this());
        if (it == _manager->_timers.end())
        {
            return false;
        }

        // 删除在插入确保时间有序
        _manager->_timers.erase(it);
        _next = system_clock::now() + milliseconds(_ms);
        _manager->_timers.insert(shared_from_this());
        return true;
    }

    // 重置定时器---重置执行时间或终止时间
    bool Timer::reset(uint64_t ms, bool from_now)
    {
        if (ms == _ms && !from_now)
        {
            return true;
        }

        {
            unique_lock<shared_mutex> write_lock(_manager->_mutex);

            if (!_cb)
            {
                return false;
            }

            auto it = _manager->_timers.find(shared_from_this());
            if (it == _manager->_timers.end())
            {
                return false;
            }
            _manager->_timers.erase(it);
        }

        // 是否从当前时间开始，否则从原起始时间开始
        auto start = from_now ? system_clock::now() : _next - milliseconds(_ms);
        _ms = ms;
        _next = start + milliseconds(ms);
        _manager->addTimer(shared_from_this()); // insert with lock
        return true;
    }

    Timer::Timer(uint64_t ms, func cb, bool recurring, TimerManager *manager)
        : _ms(ms), _cb(cb), _recurring(recurring), _manager(manager)
    {
        auto now = system_clock::now();
        _next = now + milliseconds(_ms);
    }

    bool Timer::Comparator::operator()(const shared_ptr<Timer> &lhs, const shared_ptr<Timer> &rhs) const
    {
        assert(lhs != nullptr && rhs != nullptr);
        return lhs->_next < rhs->_next;
    }

    TimerManager::TimerManager()
    {
        _previouseTime = system_clock::now();
    }

    TimerManager::~TimerManager()
    {
    }

    shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, function<void()> cb, bool recurring)
    {
        shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
        addTimer(timer);
        return timer;
    }

    // 如果条件存在 -> 执行cb()
    static void OnTimer(weak_ptr<void> weak_cond, function<void()> cb)
    {
        shared_ptr<void> tmp = weak_cond.lock();
        if (tmp)
        {
            cb();
        }
    }

    shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, function<void()> cb, weak_ptr<void> weak_cond, bool recurring)
    {
        return addTimer(ms, bind(&OnTimer, weak_cond, cb), recurring);
    }

    uint64_t TimerManager::getNextTimer()
    {
        shared_lock<shared_mutex> read_lock(_mutex);

        _tickled = false;
        if (_timers.empty())
        {
            // 返回ull最大值
            return ~0ull;
        }

        auto now = system_clock::now();
        auto time = (*_timers.begin())->_next;

        // now >= time 已经有timer超时
        if (now >= time)
        {
            return 0;
        }
        else
        {
            auto duration = duration_cast<milliseconds>(time - now);
            return static_cast<uint64_t>(duration.count());
        }
    }

    void TimerManager::listExpiredCb(vector<func> &cbs)
    {
        auto now = system_clock::now();

        unique_lock<shared_mutex> write_lock(_mutex);
        // 是否发生回退
        bool rollover = detectClockRollover();

        // 回退 -> 清理所有timer || 超时 -> 清理超时timer
        while (!_timers.empty() && (rollover || (*_timers.begin())->_next <= now))
        {
            shared_ptr<Timer> temp = *_timers.begin();
            // 清除timer
            _timers.erase(_timers.begin());

            cbs.push_back(temp->_cb);

            if (temp->_recurring)
            {
                // 重新加入时间堆
                temp->_next = now + milliseconds(temp->_ms);
                _timers.insert(temp);
            }
            else
            {
                // 清理cb
                temp->_cb = nullptr;
            }
        }
    }

    bool TimerManager::hasTimer()
    {
        shared_lock<shared_mutex> read_lock(_mutex);
        return !_timers.empty();
    }

    // lock + tickle()
    void TimerManager::addTimer(shared_ptr<Timer> timer)
    {
        bool at_front = false;
        {
            unique_lock<shared_mutex> write_lock(_mutex);
            auto it = _timers.insert(timer).first;
            at_front = (it == _timers.begin()) && !_tickled;

            // only tickle once till one thread wakes up and runs getNextTime()
            if (at_front)
            {
                _tickled = true;
            }
        }

        if (at_front)
        {
            onTimerInsertedAtFront();
        }
    }

    bool TimerManager::detectClockRollover()
    {
        bool rollover = false;
        auto now = system_clock::now();
        if (now < (_previouseTime - milliseconds(60 * 60 * 1000)))
        {
            rollover = true;
        }
        _previouseTime = now;
        return rollover;
    }

}
