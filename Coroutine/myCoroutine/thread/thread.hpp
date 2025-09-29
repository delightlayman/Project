#ifndef _THREAD_H_
#define _THREAD_H_

#include <string>
#include <functional>
#include <condition_variable>

#include <iostream>
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>

using std::cout, std::endl;

using std::condition_variable;
using std::mutex;
using std::unique_lock;

using std::function;
using std::string;

namespace myCoroutine
{
    // 信号量
    class Semaphore
    {
    private:
        int _sem_num;
        mutex _mtx;
        condition_variable _cv;

    public:
        // 信号量（资源）初始化为0
        // explicit防止隐式类型转换，使得某些情况下数值3转换为Semaphore类型
        explicit Semaphore(int sem_num = 0) : _sem_num(sem_num) {}
        // p操作---获取资源，没有资源则等待
        void wait()
        {
            //_cv.wait需要解锁加锁 lock_guard不允许手动加锁解锁，故不适用
            unique_lock<mutex> lock(_mtx);
            // while防止虚假唤醒
            while (_sem_num == 0)
            {
                _cv.wait(lock);
            }
            --_sem_num;
        }
        // v操作---释放资源，唤醒等待资源的线程
        // 操作是否等待，只看 “是否需要请求资源”，v操作不请求，所以不等待
        // 信号量值无 “上限约束”：初始化值是 “初始资源数”，不是 “不可逾越的上限”

        void signal()
        {
            unique_lock<mutex> lock(_mtx);
            ++_sem_num;
            _cv.notify_one();
        }
    };

    class Thread
    {
    public:
        using func = function<void()>;

    public:
        // 构造与析构
        Thread(const func &rt, const string &name);
        ~Thread();
        // thread信息访问
        const pid_t &getstid() const { return _stid; }
        const pthread_t &gettid() const { return _tid; }

        // 静态方法，用以快速访问当前线程
        static const string &getName();
        static void setName(const string &name);
        static pid_t getSysTid(); // 获取当前线程的内核线程id
        static Thread *getThis(); // 获取当前线程对象的指针
        // 线程操作
        void join(); // 等待线程结束
    private:
        // 线程例行函数
        static void *routine(void *arg);

    private:
        // Linux采用 轻量级进程 实现线程，内核中并不区分进程和线程，统一通过task_struct结构体管理
        // 内核态线程id : pid_t---系统全局内唯一，本质：内核中task_struct的pid字段
        // 用户态线程id : pthread_t---进程内唯一
        // 两者一一对应
        pid_t _stid = -1;   // 内核/系统线程id
        pthread_t _tid = 0; // 线程id
        string _name;       // 线程名
        func _cb;           // 例行函数
        Semaphore _sem;     // 信号量
    };
}

#endif