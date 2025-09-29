#include <iostream>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
using namespace std;

void *routine(void *arg)
{
    pthread_detach(pthread_self());
    //设置线程名称---线程较多时方便调试
    // pthread_setname_np(pthread_self(),"thread_name");
    cout << "thread id: " << pthread_self() << endl;
    cout << "thread sys id " << syscall(SYS_gettid) << endl;
    sleep(30);
    return 0;
}

int main()
{
    pthread_t parr[5];
    for (int i = 0; i < 5; i++)
    {
        pthread_create(&parr[i], nullptr, routine, nullptr);
        sleep(1);
    }
    for (int i = 0; i < 5; i++)
    {
        cout<<hex<<parr[i]<<" "<<endl;
    }
    sleep(60);
    return 0;
}
