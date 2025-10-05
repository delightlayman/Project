#ifndef _FD_MANAGER_H_
#define _FD_MANAGER_H_

#include <memory>
#include <shared_mutex>
#include "../thread/thread.hpp"

namespace myCoroutine
{

    // fd info
    class FdCtx : public std::enable_shared_from_this<FdCtx>
    {
    private:
        int _fd;
        // 是否初始化
        bool _isInit = false;
        // 是否是socket
        bool _isSocket = false;
        // 是否hook非阻塞
        bool _sysNonblock = false;
        // 是否用户设置非阻塞
        bool _userNonblock = false;
        // 是否关闭
        bool _isClosed = false;

        // 读超时时间---毫秒
        uint64_t _recvTimeout = (uint64_t)-1;
        // 写超时时间---毫秒
        uint64_t _sendTimeout = (uint64_t)-1;

    public:
        FdCtx(int fd);
        ~FdCtx();

        bool init();
        bool isInit() const { return _isInit; }
        bool isSocket() const { return _isSocket; }
        bool isClosed() const { return _isClosed; }

        void setUserNonblock(bool v) { _userNonblock = v; }
        bool getUserNonblock() const { return _userNonblock; }

        void setSysNonblock(bool v) { _sysNonblock = v; }
        bool getSysNonblock() const { return _sysNonblock; }

        void setTimeout(int type, uint64_t v);
        uint64_t getTimeout(int type);
    };

    class FdManager
    {
    public:
        FdManager();

        std::shared_ptr<FdCtx> get(int fd, bool auto_create = false);
        void del(int fd);

    private:
        std::shared_mutex _mutex;
        std::vector<std::shared_ptr<FdCtx>> _datas;
    };

    template <typename T>
    class Singleton
    {
    private:
        static T *instance;
        static std::mutex mutex;

    protected:
        Singleton() {}

    public:
        // Delete copy constructor and assignment operation
        Singleton(const Singleton &) = delete;
        Singleton &operator=(const Singleton &) = delete;

        static T *GetInstance()
        {
            std::lock_guard<std::mutex> lock(mutex); // Ensure thread safety
            if (instance == nullptr)
            {
                instance = new T();
            }
            return instance;
        }

        static void DestroyInstance()
        {
            std::lock_guard<std::mutex> lock(mutex);
            delete instance;
            instance = nullptr;
        }
    };

    typedef Singleton<FdManager> FdMgr;

}

#endif