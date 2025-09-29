#include "thread.hpp"
#include <iostream>
#include <unistd.h>
#include <vector>
#include <memory>
#include <string>
using namespace myCoroutine;
using namespace std;
void callback()
{
    cout << "cur id : " << Thread::getSysTid() << ", cur name : " << Thread::getName() << endl;
    cout << "this stid : " << Thread::getThis()->getstid() << ", this tid : " << hex << Thread::getThis()->gettid() << dec << endl;

    sleep(10);
}

int main()
{
    vector<shared_ptr<Thread>> tarr(5);
    for (int i = 0; i < 5; i++)
    {
        // make_shared一次性分配一块连续内存，同时容纳对象和控制块,而非分两次分配
        tarr[i] = make_shared<Thread>(callback, "thread" + to_string(i));
    }

    // for (int i = 0; i < 5; i++)
    // {
    //     tarr[i]->join();
    // }
    sleep(15);
    return 0;
}