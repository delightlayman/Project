
半同步/半反应堆线程池
===============
使用一个工作队列完全解除了主线程和工作线程的耦合关系：主线程往工作队列中插入任务，工作线程通过竞争来取得任务并执行它。
> * 同步I/O模拟proactor模式
> * 半同步/半反应堆
> * 线程池

## 标准异常类
基类：std::exception（所有标准异常的根类，提供what()方法返回错误描述）
std::exception派生类：
- std::bad_alloc：内存分配失败（如new操作失败）
- std::bad_cast：动态类型转换失败（如dynamic_cast对引用转换失败）
- std::bad_typeid：对空指针使用typeid操作符
- std::bad_exception：用于异常规范中，标识意外未捕获的异常
- std::logic_error：逻辑错误（编译 / 设计阶段可避免的错误）
std::logic_error派生类：
  - std::domain_error：参数超出有效定义域（如数学函数输入不合法）
  - std::invalid_argument：无效参数（如向函数传递空指针）
  - std::length_error：长度超出允许范围（如字符串长度超限）
  - std::out_of_range：访问范围外元素（如容器at()方法越界）
- std::runtime_error：运行时错误（仅在运行时可检测的错误）
std::runtime_error派生类：
  - std::range_error：计算结果超出有效范围（如统计结果溢出）
  - std::overflow_error：算术运算上溢（如整数加法超出最大值）
  - std::underflow_error：算术运算下溢（如浮点数减法超出最小值）
  - std::system_error：系统调用或系统操作失败（封装系统错误码，如errno）
  std::system_error常用错误类型：
    - std::generic_category()：对应 C 标准库（如<cstdio>、<cmath>等）中的错误码，即<cerrno>中定义的通用错误（如EINVAL、EDOM、ERANGE等）。
    - std::system_category()：对应操作系统特定的系统调用错误（如 Linux 的open、read，Windows 的CreateFile等系统调用失败时的错误码）。

## 异常处理过程
C++ 异常处理是一种在程序运行时检测和响应错误的机制，核心目标是将 "检测错误" 和 "处理错误" 的代码分离，提高程序的健壮性和可读性。其全过程可分为异常抛出、异常传播、异常捕获和程序恢复四个阶段，以下是详细解析：
### 一、异常的抛出（Throw）
当程序执行过程中遇到无法处理的错误（如除以零、内存分配失败、数组越界等），通过throw语句主动 "抛出" 异常(**事发前的主动检测**)，标志着异常情况的发生。
- 关键特性：
    - 抛出的数据类型：可以是基本类型（如int、double）、自定义对象（最常用）、指针等，推荐抛出对象（可携带更多错误信息）。
    - 抛出点：throw语句执行后，当前函数的正常执行会立即终止，程序进入 "异常处理模式"。
- 示例：
```cpp
void divide(int a, int b) {
    if (b == 0) {
        // 抛出异常（此处为字符串字面量，实际推荐自定义异常类）
        throw "除数不能为零"; 
    }
    cout << "结果：" << a / b << endl;
}
```
### 二、异常的传播（Propagation）
异常抛出后，程序不会立即终止，而是沿着函数调用栈向上传播，寻找能够处理该异常的代码（即catch块）。这个过程伴随 "**栈展开**（Stack Unwinding）"。
- 栈展开（核心机制）：
当异常传播时，程序会逐层退出当前的函数调用栈，销毁栈中已创建的局部对象（调用其析构函数），直到找到匹配的catch块。这确保了资源（如内存、文件句柄）被正确释放（与 RAII 机制配合时尤为重要）。
- 示例场景：
```cpp
void func3() { throw 100; } // 抛出异常
void func2() { func3(); }    // 调用func3，无异常处理
void func1() { func2(); }    // 调用func2，无异常处理

int main() {
    try {
        func1(); // 调用func1
    } catch (int e) {
        cout << "捕获异常：" << e << endl;
    }
}
```
- 传播路径：func3() → func2() → func1() → main()的try块。
- 栈展开：退出func3时销毁其局部对象 → 退出func2时销毁其局部对象 → 退出func1时销毁其局部对象 → 到达main的try块。
### 三、异常的捕获（Catch）
异常传播到try块后，程序会检查try块后的catch块，寻找与异常类型匹配的处理代码。
- 匹配规则：
    - catch块的参数类型需与抛出的异常类型完全匹配（或存在可接受的隐式转换，如派生类对象可匹配基类引用）。
    - 若有多个catch块，按声明顺序匹配，匹配成功后不再检查后续catch块。
    - 可使用catch(...)捕获所有类型的异常（通常作为 "最后防线"，避免未处理的异常导致程序终止）。
- 示例：
```cpp
try {
    divide(10, 0); // 可能抛出异常的代码
} catch (const char* msg) { // 匹配"字符串字面量"类型的异常
    cout << "错误信息：" << msg << endl;
} catch (...) { // 捕获所有其他未匹配的异常
    cout << "发生未知异常" << endl;
}
```
### 四、程序恢复（Resume）
**异常被捕获并处理后，程序会从最后一个catch块的末尾继续执行**，而非回到异常抛出的位置（与 goto 不同，异常处理是 "非局部跳转"）。
- 示例流程：
```cpp
int main() {
    cout << "程序开始" << endl;
    try {
        divide(10, 0); 
        cout << "除法完成" << endl; // 异常抛出后，此句不会执行
    } catch (const char* msg) {
        cout << "处理异常：" << msg << endl; // 执行异常处理
    }
    cout << "程序继续执行" << endl; // 异常处理后，从此处恢复
    return 0;
}
```
- 输出：
```bash
程序开始
处理异常：除数不能为零
程序继续执行
```
### 五、关键补充概念
1. 标准异常类：C++ 标准库提供了一套异常类层次结构（以std::exception为基类），如：
- std::runtime_error：运行时错误（如文件未找到）
- std::out_of_range：容器访问越界
- std::bad_alloc：new分配内存失败时抛出
推荐使用标准异常类或其派生类，便于统一处理。
- 示例：
```cpp
#include <stdexcept>
void checkAge(int age) {
    if (age < 0) {
        throw std::invalid_argument("年龄不能为负数"); // 标准异常
    }
}
```
2. 异常规格说明（已过时）：C++98 中用throw(类型列表)声明函数可能抛出的异常类型（如void func() throw(int, char)），但 C++11 后被noexcept替代（noexcept表示函数不抛出任何异常）。
3. **未捕获的异常：若异常传播至程序入口点（如main函数）仍未被捕获，会调用std::terminate()终止程序（默认行为是调用abort()）**