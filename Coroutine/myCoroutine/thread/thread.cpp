#include "thread.hpp"

namespace myCoroutine
{
    // 当前线程信息---提供当前线程的快捷访问方式
    // thread_local---线程安全
    static thread_local Thread *t_thread = nullptr;
    static thread_local string t_thread_name = "UNKNOW";

    Thread::Thread(const func &rt, const string &name) : _cb(rt), _name(name)
    {
        int res = pthread_create(&_tid, nullptr, &Thread::routine, this);
        if (res != 0)
        {
            cout << "pthread_create failed,res = " << res << "thread name" << name << endl;
            throw std::logic_error("pthread_create failed");
        }
        // 等待线程初始化完成---避免线程还未初始化完成，外部访问此线程，获取错误信息
        _sem.wait();
    }
    Thread::~Thread()
    {
        if (_tid != 0)
        {
            pthread_detach(_tid);
            _tid = 0;
            // cout << "detach" << endl;
        }
        // else
        //     cout << "no need detach" << endl;
    }
    const string &Thread::getName()
    {
        return t_thread_name;
    }
    void Thread::setName(const string &name)
    {
        if (t_thread)
        {
            t_thread->_name = name;
        }
        t_thread_name = name;
    }
    pid_t Thread::getSysTid()
    {
        return syscall(SYS_gettid); // syscall系统调用，SYS_gettid为调用编号
    }
    Thread *Thread::getThis()
    {
        return t_thread;
    }
    void *Thread::routine(void *arg)
    {
        Thread *td = (Thread *)arg;
        // 线程初始化
        t_thread = td;
        t_thread_name = td->_name;
        td->_stid = getSysTid();
        // 设置线程名称---线程较多时方便调试 ps -L 的CMD列查看
        // 线程最长15字节+‘\0’
        pthread_setname_np(pthread_self(), td->_name.substr(0, 15).c_str());

        // 交换而非复制优点：
        // 1.减少复制带来的开销
        // 2.减少不必要的引用计数操作
        // 3.避免Thread对象生命周期及其回调函数_cb的执行周期同步问题
        //    ---针对函数对象，lambda,不适用普通函数，因前两者执行完毕时，仍可能持有资源
        //    若Thread对象析构-->_cb被析构，routine中再执行_cb则会异常
        //    若_cb执行结束，Thread对象未析构-->_cb未析构，即_cb一直持有资源无法释放，尤其lambda捕获的shared_ptr
        func cb;          // 空函数包装器
        cb.swap(td->_cb); // 交换两个函数对象

        // 线程创建完成
        td->_sem.signal();
        // 执行回调函数
        cb();
        return 0;
    }
    void Thread::join()
    {
        if (_tid)
        {
            int res = pthread_join(_tid, nullptr);
            if (res)
            {
                cout << "pthread_join failed,res = " << res << "thread name" << _name << endl;
                throw std::logic_error("pthread_join failed");
            }
            _tid = 0;
            // cout << "join" << endl;
        }
        // else
        //     cout << "no need join" << endl;
    }
}
