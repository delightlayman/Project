1. std::enable_shared_from_this<Fiber> 的核心作用是：
    让 Fiber 类的对象在被 shared_ptr 管理时，能安全地获取指向自身的 shared_ptr，确保引用计数正确，避免内存错误。
适用将对象自身作为 shared_ptr 传递的场景（如回调、事件注册
----当 Fiber 继承 std::enable_shared_from_this<Fiber> 后，类内部可以通过 shared_from_this() 成员函数
    获取一个指向自身的 shared_ptr，这个 shared_ptr 会与已有的管理该对象的 shared_ptr 共享引用计数，从而避免重复释放
----std::shared_ptr<Fiber> ptr(this) 计数不共享的根本原因是：this 作为原始指针，无法关联到对象已有的控制块，
    导致新创建的 shared_ptr 会生成独立的控制块，与原有 shared_ptr 的控制块无关