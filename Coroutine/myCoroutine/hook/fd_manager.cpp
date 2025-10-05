#include "fd_manager.hpp"
#include "hook.hpp"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace myCoroutine{

// instantiate---显式实例化---全局唯一的单例实例
template class Singleton<FdManager>;

// Static variables need to be defined outside the class
template<typename T>
T* Singleton<T>::instance = nullptr;

template<typename T>
std::mutex Singleton<T>::mutex;	

FdCtx::FdCtx(int fd):
_fd(fd)
{
	init();
}

FdCtx::~FdCtx()
{

}

bool FdCtx::init()
{
	if(_isInit)
	{
		return true;
	}
	
	struct stat statbuf;
	// fd is in valid
    // fstat用于获取fd关联文件的文件状态信息，存入statbuf
	if(-1==fstat(_fd, &statbuf))
	{
		_isInit = false;
		_isSocket = false;
	}
	else
	{
		_isInit = true;	
        // 判定文件类型是否是socket---S_ISSOCK宏
		_isSocket = S_ISSOCK(statbuf.st_mode);	
	}

	// if it is a socket -> set to nonblock
	if(_isSocket)
	{
		// fcntl_f() -> the original fcntl() -> get the file status flags
		int flags = fcntl_f(_fd, F_GETFL, 0);
		if(!(flags & O_NONBLOCK))
		{
			// if not -> status flags append nonblock
			fcntl_f(_fd, F_SETFL, flags | O_NONBLOCK);
		}
		_sysNonblock = true;
	}
	else
	{
		_sysNonblock = false;
	}

	return _isInit;
}

//type: 超时类型标志：SO_RCVTIMEO---接收超时，SO_SNDTIMEO---发送超时
void FdCtx::setTimeout(int type, uint64_t v)
{
	if(type==SO_RCVTIMEO)
	{
		_recvTimeout = v;
	}
	else
	{
		_sendTimeout = v;
	}
}

uint64_t FdCtx::getTimeout(int type)
{
	if(type==SO_RCVTIMEO)
	{
		return _recvTimeout;
	}
	else
	{
		return _sendTimeout;
	}
}

FdManager::FdManager()
{
	_datas.resize(64);
}

std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create)
{
	if(fd==-1)
	{
		return nullptr;
	}
    //auto_create==false---不需要自动创建
	std::shared_lock<std::shared_mutex> read_lock(_mutex);
	if(_datas.size() <= fd)
	{
		if(auto_create==false)
		{
			return nullptr;
		}
	}
	else
	{
		if(_datas[fd]||!auto_create)
		{
			return _datas[fd];
		}
	}
	read_lock.unlock();
    //auto_create==true---需要自动创建
	std::unique_lock<std::shared_mutex> write_lock(_mutex);

	if(_datas.size() <= fd)
	{
		_datas.resize(fd*1.5);
	}

	_datas[fd] = std::make_shared<FdCtx>(fd);
	return _datas[fd];

}

void FdManager::del(int fd)
{
	std::unique_lock<std::shared_mutex> write_lock(_mutex);
	if(_datas.size() <= fd)
	{
		return;
	}
	_datas[fd].reset();
}

}