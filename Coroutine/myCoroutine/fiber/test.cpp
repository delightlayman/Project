#include "fiber.hpp"
#include <iostream>
#include <vector>
using namespace std;
using namespace myCoroutine;
class scheduler
{
public:
    using value_type = shared_ptr<Fiber>;
    // 添加协程
    void add(value_type f)
    {
        _fibers.push_back(f);
        _size++;
    }
    // 运行所有协程
    void run()
    {
        for (int i = 0; i < _size; i++)
        {
            _fibers[i]->resume();
        }
        _fibers.clear();
        _size = 0;
    }

private:
    vector<value_type> _fibers;
    size_t _size = 0;
};

void fiber_cb(int i)
{
    cout << "fiber_cb : " << i << endl;
}

int main()
{
    // 创建并启动主协程
    Fiber::getThis();
    scheduler sd;
    for (int i = 0; i < 50; i++){
        sd.add(make_shared<Fiber>(bind(fiber_cb, i)));
    }
    sd.run();
    return 0;
}