std::shared_mutex 实现读写锁（Reader-Writer Lock）机制，适用于读多写少的场景。
1.两种访问模式：
    共享模式（读操作）：多个线程可以同时获取共享锁，并发读取数据
    独占模式（写操作）：只有一个线程能获取独占锁，用于修改数据
2.主要特点：
    当没有线程持有独占锁时，多个线程可以同时获得共享锁（支持并发读）
    当有线程持有共享锁时，试图获取独占锁的线程会阻塞，直到所有共享锁被释放
    当有线程持有独占锁时，其他线程（无论想获取共享锁还是独占锁）都会阻塞
3.典型用法：
使用 std::shared_lock<std::shared_mutex> 获取共享锁（读操作）
使用 std::unique_lock<std::shared_mutex> 或 std::lock_guard<std::shared_mutex> 获取独占锁（写操作）

chrono:时间管理工具
1.时钟类型（clock）
chrono 提供了三种时钟类型，用于获取当前时间：
    system_clock：系统时钟，可转换为日历时间（如年月日）
    steady_clock：稳定时钟，时间不会回退（适合测量时间间隔）
    high_resolution_clock：高精度时钟（通常是 steady_clock 或 system_clock 的别名）
2.时间点（time_Point）
时间点模板参数：时钟类型，时间间隔类型，默认时钟类型的间隔类型
    template<class Clock, class Duration = typename Clock::duration>
    class time_point;
时间点表示一个具体的时间点，chrono 提供了 time_point 类型表示时间点：
    auto now = std::chrono::system_clock::now(); // 获取当前时间点

3.时间间隔（duration）
时间间隔模板参数：时间存储的底层数值类型，时间间隔的单位，默认为秒
template<class Rep, class Period = std::ratio<1>>
class duration;
常用：
    时间间隔表示两个时间点之间的时间差。chrono 提供的 duration 类型表示时间间隔(包括但不限于)：
    std::chrono::seconds：秒
    std::chrono::milliseconds：毫秒
    std::chrono::microseconds：微秒
    std::chrono::nanoseconds：纳秒
    auto duration = std::chrono::seconds(5); // 5秒

weak_ptr<void>
    weak_ptr<void> 可以指向任意类型的对象（类似 void* 可以指向任意类型的原始指针），但依然保持 “弱引用” 的特性：
        不影响对象的生命周期（不会阻止 shared_ptr 释放对象）；
        可以通过 lock() 方法尝试将其转换为 shared_ptr<T>（需显式指定原始类型 T），若对象已被释放则返回空的 shared_ptr。
    weak_ptr<void> 本身不能直接用于条件判断,可通过 lock() 尝试获取一个 shared_ptr<void>判断