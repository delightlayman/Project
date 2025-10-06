### hook概述
#### 理解hook
- hook实际上就是对系统调用API进行一次封装，将其封装成一个与原始的系统调用API同名的接口，应用在调用这个接口时，会先执行封装中的操作，再执行原始的系统调用API。

- hook技术可以使应用程序在执行系统调用之前进行一些隐藏的操作，比如可以对系统提供malloc()和free()进行hook，在真正进行内存分配和释放之前，统计内存的引用计数，以排查内存泄露问题。

- 还可以用C++的子类重载来理解hook。在C++中，子类在重载父类的同名方法时，一种常见的实现方式是子类先完成自己的操作，再调用父类的操作，如下：
```c++
class Base {
public:
    void Print() {
        cout << "This is Base" << endl;
    }
};

class Child : public Base {
public:
    /// 子类重载时先实现自己的操作，再调用父类的操作
    void Print() {
        cout << "This is Child" << endl;
        Base::Print();
    }
};
```
- 在上面的代码实现中，调用子类的Print方法，会先执行子类的语句，然后再调用父类的Print方法，这就相当于子类hook了父类的Print方法。 

- 由于hook之后的系统调用与原始的系统系统调用同名，所以对于程序开发者来说也很方便，不需要重新学习新的接口，只需要按老的接口调用惯例直接写代码就行了。
### hook功能
hook的目的是在不重新编写代码的情况下，把老代码中的socket IO相关的API都转成异步，以提高性能。hook和IO协程调度是密切相关的，如果不使用IO协程调度器，那hook没有任何意义，考虑IOManager要在一个线程上按顺序调度以下协程：

    协程1：sleep(2) 睡眠两秒后返回。
    协程2：在scoket fd1 上send 100k数据。
    协程3：在socket fd2 上recv直到数据接收成功。
1. 在未hook的情况下，IOManager要调度上面的协程，流程是下面这样的：

- 调度协程1，协程阻塞在sleep上，等2秒后返回，这两秒内调度线程是被协程1占用的，其他协程无法在当前线程上调度。
- 调度协徎2，协程阻塞send 100k数据上，这个操作一般问题不大，因为send数据无论如何都要占用时间，但如果fd迟迟不可写，那send会阻塞直到套接字可写，同样，在阻塞期间，其他协程也无法在当前线程上调度。
- 调度协程3，协程阻塞在recv上，这个操作要直到recv超时或是有数据时才返回，期间调度器也无法调度其他协程。  
  
上面的调度流程最终总结起来就是，协程只能按顺序调度，一旦有一个协程阻塞住了，那整个调度线程也就阻塞住了，其他的协程都无法在当前线程上执行。像这种一条路走到黑的方式其实并不是完全不可避免，以sleep为例，调度器完全可以在检测到协程sleep后，将协程yield以让出执行权，同时设置一个定时器，2秒后再将协程重新resume。这样，调度器就可以在这2秒期间调度其他的任务，同时还可以顺利的实现sleep 2秒后再继续执行协程的效果，send/recv与此类似。
2. 在完全实现hook后，IOManager的执行流程将变成下面的方式：

- 调度协程1，检测到协程sleep，那么先添加一个2秒的定时器，定时器回调函数是在调度器上继续调度本协程，接着协程yield，等定时器超时。
- 因为上一步协程1已经yield了，所以协徎2并不需要等2秒后才可以执行，而是立刻可以执行。同样，调度器检测到协程send，由于不知道fd是不是马上可写，所以先在IOManager上给fd注册一个写事件，回调函数是让当前协程resume并执行实际的send操作，然后当前协程yield，等可写事件发生。
- 上一步协徎2也yield了，可以马上调度协程3。协程3与协程2类似，也是给fd注册一个读事件，回调函数是让当前协程resume并继续recv，然后本协程yield，等事件发生。
- 等2秒超时后，执行定时器回调函数，将协程1 resume以便继续执行。
- 等协程2的fd可写，一旦可写，调用写事件回调函数将协程2 resume以便继续执行send。
- 等协程3的fd可读，一旦可读，调用回调函数将协程3 resume以便继续执行recv。
上面的4、5、6步都是异步的，调度线程并不会阻塞，IOManager仍然可以调度其他的任务，只在相关的事件发生后，再继续执行对应的任务即可。并且，由于hook的函数签名与原函数一样，所以对调用方也很方便，只需要以同步的方式编写代码，实现的效果却是异步执行的，效率很高。

3. 总而言之，在IO协程调度中对相关的系统调用进行hook，可以让调度线程尽可能得把时间片都花在有意义的操作上，而不是浪费在阻塞等待中。

hook的重点是在替换API的底层实现的同时完全模拟其原本的行为，因为调用方是不知道hook的细节的，在调用被hook的API时，如果其行为与原本的行为不一致，就会给调用方造成困惑。比如，所有的socket fd在进行IO调度时都会被设置成NONBLOCK模式，如果用户未显式地对fd设置NONBLOCK，那就要处理好fcntl，不要对用户暴露fd已经是NONBLOCK的事实，这点也说明，除了IO相关的函数要进行hook外，对fcntl, setsockopt之类的功能函数也要进行hook，才能保证API的一致性。
### hook实现
这里只讲解动态链接中的hook实现，静态链接以及基于内核模块的hook不在本章讨论范围。

在学习hook之前需要对Linux的动态链接有一定的了解，建议阅读《程序员的自我修养 —— 链接、装载与库》第7章。本站 关于[链接与装载的几个测试代码](https://www.midlane.top/wiki/pages/viewpage.action?pageId=16418206) 提供了一些示例，有助于理解动态链接的具体行为。

hook的实现机制非常简单，就是通过动态库的全局符号介入功能，用自定义的接口来替换掉同名的系统调用接口。由于系统调用接口基本上是由C标准函数库libc提供的，所以这里要做的事情就是用自定义的动态库来覆盖掉libc中的同名符号。

基于动态链接的hook有两种方式，第一种是外挂式hook，也称为非侵入式hook，通过优先加自定义载动态库来实现对后加载的动态库进行hook，这种hook方式不需要重新编译代码，考虑以下例子：
```c++
#include <unistd.h>
#include <string.h>
 
int main() {
    write(STDOUT_FILENO, "hello world\n", strlen("hello world\n")); // 调用系统调用write写标准输出文件描述符
    return 0;
}
```
在这个例子中，可执行程序调用write向标准输出文件描述符写数据。对这个程序进行编译和执行，效果如下：

```bash
# gcc main.c
# ./a.out
hello world
```
使用ldd命令查看可执行程序的依赖的共享库，如下：
```bash
# ldd a.out
        linux-vdso.so.1 (0x00007ffc96519000)
        libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007fda40a61000)
        /lib64/ld-linux-x86-64.so.2 (0x00007fda40c62000)
```
可以看到其依赖libc共享库，write系统调用就是由libc提供的。

gcc编译生成可执行文件时会默认链接libc库，所以不需要显式指定链接参数，这点可以在编译时给 gcc 增加一个 "-v" 参数，将整个编译流程详细地打印出来进行验证，如下：
```bash
# gcc -v main.c
Using built-in specs.
COLLECT_GCC=gcc
COLLECT_LTO_WRAPPER=/usr/lib/gcc/x86_64-linux-gnu/9/lto-wrapper
OFFLOAD_TARGET_NAMES=nvptx-none:hsa
OFFLOAD_TARGET_DEFAULT=1
Target: x86_64-linux-gnu
...
 /usr/lib/gcc/x86_64-linux-gnu/9/collect2 -plugin /usr/lib/gcc/x86_64-linux-gnu/9/liblto_plugin.so -plugin-opt=/usr/lib/gcc/x86_64-linux-gnu/9/lto-wrapper -plugin-opt=-fresolution=/tmp/ccZQ60eg.res -plugin-opt=-pass-through=-lgcc -plugin-opt=-pass-through=-lgcc_s -plugin-opt=-pass-through=-lc -plugin-opt=-pass-through=-lgcc -plugin-opt=-pass-through=-lgcc_s --build-id --eh-frame-hdr -m elf_x86_64 --hash-style=gnu --as-needed -dynamic-linker /lib64/ld-linux-x86-64.so.2 -pie -z now -z relro /usr/lib/gcc/x86_64-linux-gnu/9/../../../x86_64-linux-gnu/Scrt1.o /usr/lib/gcc/x86_64-linux-gnu/9/../../../x86_64-linux-gnu/crti.o /usr/lib/gcc/x86_64-linux-gnu/9/crtbeginS.o -L/usr/lib/gcc/x86_64-linux-gnu/9 -L/usr/lib/gcc/x86_64-linux-gnu/9/../../../x86_64-linux-gnu -L/usr/lib/gcc/x86_64-linux-gnu/9/../../../../lib -L/lib/x86_64-linux-gnu -L/lib/../lib -L/usr/lib/x86_64-linux-gnu -L/usr/lib/../lib -L/usr/lib/gcc/x86_64-linux-gnu/9/../../.. /tmp/ccnT2NOd.o -lgcc --push-state --as-needed -lgcc_s --pop-state -lc -lgcc --push-state --as-needed -lgcc_s --pop-state /usr/lib/gcc/x86_64-linux-gnu/9/crtendS.o /usr/lib/gcc/x86_64-linux-gnu/9/../../../x86_64-linux-gnu/crtn.o
COLLECT_GCC_OPTIONS='-v' '-mtune=generic' '-march=x86-64'
```
下面在不重新编译代码的情况下，用自定义的动态库来替换掉可执行程序a.out中的write实现，新建hook.c，内容如下：
```c++
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
 
ssize_t write(int fd, const void *buf, size_t count) {
    syscall(SYS_write, STDOUT_FILENO, "12345\n", strlen("12345\n"));
}
```
这里实现了一个write函数，这个函数的签名和libc提供的write函数完全一样，函数内容是用syscall的方式直接调用编号为SYS_write的系统调用，实现的效果也是往标准输出写内容，只不过这里我们将输出内容替换成了其他值。将hook.c编译成动态库：
```bash
gcc -fPIC -shared hook.c -o libhook.so
```
通过设置 LD_PRELOAD环境变量，将libhook.so设置成优先加载，从面覆盖掉libc中的write函数，如下：
```bash
# LD_PRELOAD="./libhook.so" ./a.out
12345
```
这里我们并没有重新编译可执行程序a.out，但是可以看到，write的实现已经替换成了我们自己的实现。究其原因，就是LD_PRELOAD环境变量，它指明了在运行a.out之前，系统会优先把libhook.so加载到了程序的进程空间，使得在a.out运行之前，其全局符号表中就已经有了一个write符号，这样在后续加载libc共享库时，由于全局符号介入机制，libc中的write符号不会再被加入全局符号表，所以全局符号表中的write就变成了我们自己的实现。

第二种方式的hook是侵入式的，需要改造代码或是重新编译一次以指定动态库加载顺序。如果是以改造代码的方式来实现hook，那么可以像下面这样直接将write函数的实现放在main.c里，那么编译时全局符号表里先出现的必然是main.c中的write符号：
```c++
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>
 
ssize_t write(int fd, const void *buf, size_t count) {
    syscall(SYS_write, STDOUT_FILENO, "12345\n", strlen("12345\n"));
}
 
int main() {
    write(STDOUT_FILENO, "hello world\n", strlen("hello world\n")); // 这里调用的是上面的write实现
    return 0;
}
```
如果不改造代码，那么可以重新编译一次，通过编译参数将自定义的动态库放在libc之前进行链接。由于默认情况下gcc总会链接一次libc，并且libc的位置也总在命令行所有参数后面，所以只需要像下面这样操作就可以了：
```bash
# gcc main.c -L. -lhook -Wl,-rpath=.
# ./a.out
12345
```  
这里显示 这里显式指定了链接 -libhook.so-（-Wl,-rpath=.用于指定运行时的动态库搜索路径，避免找不到动态库的问题），由于libhook.so的链接位置比libc要靠前（可以通过gcc -v进行验证），所以运行时会先加载 -libhook.so-，从而实现全局符号介入，这点也可以通过ldd命令来查看：
```bash
# ldd a.out
        linux-vdso.so.1 (0x00007ffe615f9000)
        libhook.so => ./libhook.so (0x00007fab4bae3000)
        libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007fab4b8e9000)
        /lib64/ld-linux-x86-64.so.2 (0x00007fab4baef000)
```
关于hook的另一个讨论点是如何找回已经被全局符号介入机制覆盖的系统调用接口，这个功能非常实用，因为大部分情况下，系统调用提供的功能都是无可替代的，我们虽然可以用hook的方式将其替换成自己的实现，但是最终要实现的功能，还是得由原始的系统调用接口来完成。

以malloc和free为例，假如我们要hook标准库提供的malloc和free接口，以跟踪每次分配和释放的内存地址，判断有无内存泄漏问题，那么具体的实现方式应该是，先调用自定义的malloc和free实现，在分配和释放内存之前，记录下内存地址，然后再调用标准库里的malloc和free，以真正实现内存申请和释放。

上面的过程涉及到了查找后加载的动态库里被覆盖的符号地址问题。首先，这个操作本身就具有合理性，因为程序运行时，依赖的动态库无论是先加载还是后加载，最终都会被加载到程序的进程空间中，也就是说，那些因为加载顺序靠后而被覆盖的符号，它们只是被“雪藏”了而已，实际还是存在于程序的进程空间中的，通过一定的办法，可以把它们再找回来。在Linux中，这个方法就是dslym，它的函数原型如下：
```c++

#define _GNU_SOURCE
#include <dlfcn.h>
 
void *dlsym(void *handle, const char *symbol);
```
关于dlsym的使用可参考man 3 dlsym，在链接时需要指定 -ldl 参数。使用dlsym找回被覆盖的符号时，第一个参数固定为 RTLD_NEXT，第二个参数为符号的名称，下面通过dlsym来实现上面的内存跟踪功能：
```c++
#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
 
typedef void* (*malloc_func_t)(size_t size);
typedef void (*free_func_t)(void *ptr);
 
// 这两个指针用于保存libc中的malloc和free的地址
malloc_func_t sys_malloc = NULL;
free_func_t sys_free = NULL;
 
// 重定义malloc和free，在这里重定义会导致libc中的同名符号被覆盖
// 这里不能调用带缓冲的printf接口，否则会出段错误
void *malloc(size_t size) {
    // 先调用标准库里的malloc申请内存，再记录内存分配信息，这里只是简单地将内存地址和长度打印出来
    void *ptr = sys_malloc(size);
    fprintf(stderr, "malloc: ptr=%p, length=%ld\n", ptr, size);
    return ptr;
}
void free(void *ptr) {
    // 打印内存释放信息，再调用标准库里的free释放内存
    fprintf(stderr, "free: ptr=%p\n", ptr);
    sys_free(ptr);
}
 
int main() {
    // 通过dlsym找到标准库中的malloc和free的符号地址
    sys_malloc = dlsym(RTLD_NEXT, "malloc");
    assert(dlerror() == NULL);
    sys_free = dlsym(RTLD_NEXT, "free");
    assert(dlerror() == NULL);
 
    char *ptrs[5];
 
    for(int i = 0; i < 5; i++) {
        ptrs[i] = malloc(100 + i);
        memset(ptrs[i], 0, 100 + i);
    }
     
    for(int i = 0; i < 5; i++) {
        free(ptrs[i]);
    }
    return 0;
}
```
编译运行以上代码，效果如下：
```bash
# gcc hook_malloc.c -ldl
# ./a.out
malloc: ptr=0x55775fa8e2a0, length=100
malloc: ptr=0x55775fa8e310, length=101
malloc: ptr=0x55775fa8e380, length=102
malloc: ptr=0x55775fa8e3f0, length=103
malloc: ptr=0x55775fa8e460, length=104
free: ptr=0x55775fa8e2a0
free: ptr=0x55775fa8e310
free: ptr=0x55775fa8e380
free: ptr=0x55775fa8e3f0
free: ptr=0x55775fa8e460
```

### 变参函数宏
<stdarg.h> 定义了 4 个关键宏，用于处理变参：
1. va_list一种类型，用于声明一个 "变参指针"，存储变参列表的信息（如当前参数位置、类型等）。
2. va_start(ap, last)初始化变参指针 ap，使其指向第一个可变参数。last 是函数中最后一个固定参数（变参必须跟在固定参数之后）。
3. va_arg(ap, type)从变参列表中获取下一个参数，type 是该参数的类型（如 int、char* 等）。**每次调用会自动更新 ap 指向的位置**。
4. va_end(ap)结束变参处理，释放 ap 相关的资源（必须在函数返回前调用）。


