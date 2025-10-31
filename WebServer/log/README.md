## 同步/异步日志系统
### 流程
![流程图](https://mmbiz.qpic.cn/mmbiz_jpg/6OkibcrXVmBEOjicsa8vpoLAlODicrC7AoM1h2eq9sDMdQY8TNYQoVckCRDd0m8SDH1myuB4gEJfejvznfZuJ3cpQ/640?wx_fmt=jpeg&tp=webp&wxfrom=5&wx_lazy=1#imgIndex=1)
- 日志文件
局部变量的懒汉模式获取实例
生成日志文件，并判断同步和异步写入方式
- 同步
判断是否分文件
直接格式化输出内容，将信息写入日志文件
- 异步
判断是否分文件
格式化输出内容，将内容写入阻塞队列，创建一个写线程，从阻塞队列取出内容写入日志文件
---------
### 简介
同步/异步日志系统主要涉及了两个模块，一个是日志模块，一个是阻塞队列模块,其中加入阻塞队列模块主要是解决异步写入日志做准备.
> * 自定义阻塞队列
>> 1. 封装了生产者-消费者模型，push成员是生产者，pop成员是消费者。
>> 2. 阻塞队列使用循环数组实现队列，作为两者共享缓冲区，队列也可以使用STL中的queue。 
> * 单例模式创建日志
> * 同步日志
> * 异步日志
> * 实现按天、超行分类

### C++11 对局部静态变量初始化的线程安全保证
```c++
    // C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }
```
C++11 标准（[stmt.dcl] 第 4 条）规定：
> 局部静态变量的初始化仅在程序执行流第一次进入其所在的作用域时进行，且仅初始化一次,且编译器必须保证这个初始化过程是线程安全的。
- 具体来说，当多个线程同时首次调用get_instance()时：
1. 只有一个线程会执行局部静态变量instance的初始化逻辑（构造函数）；
2. 其他线程会被阻塞，直到初始化完成后再继续执行；
3. 初始化完成后，后续所有调用（无论哪个线程）都会直接返回已初始化的实例指针

### 可变参数宏__VA_ARGS__
__VA_ARGS__是一个可变参数的宏，定义时宏定义中参数列表的最后一个参数为省略号，在实际使用时会发现有时会加##，有时又不加。
```c++
1//最简单的定义
2#define my_print1(...)  printf(__VA_ARGS__)
3
4//搭配va_list的format使用
5#define my_print2(format, ...) printf(format, __VA_ARGS__)  
6#define my_print3(format, ...) printf(format, ##__VA_ARGS__)
```
__VA_ARGS__宏前面加上##的作用在于，当可变参数的个数为0时，这里printf参数列表中的的##会把前面多余的","去掉，否则会编译出错，建议使用后面这种，使得程序更加健壮。

### fflush
```c++
1#include <stdio.h>
2int fflush(FILE *stream);
```
fflush()会强迫将缓冲区内的数据写回参数stream 指定的文件中，如果参数stream 为NULL，fflush()会将所有打开的文件数据更新。

> 在使用多个输出函数连续进行多次输出到控制台时，有可能下一个数据在上一个数据还没输出完毕，还在输出缓冲区中时，下一个printf就把另一个数据加入输出缓冲区，结果冲掉了原来的数据，出现输出错误。

在prinf()后加上fflush(stdout); 强制马上输出到控制台，可以避免出现上述错误。

