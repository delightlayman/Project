
定时器处理非活动连接
===============
由于非活跃连接占用了连接资源，严重影响服务器的性能，通过实现一个服务器定时器，处理这种非活跃连接，释放连接资源。利用alarm函数周期性地触发SIGALRM信号,该信号的信号处理函数利用管道通知主循环执行定时器链表上的定时任务.
> * 统一事件源
> * 基于升序链表的定时器
> * 处理非活动连接

------

## 概述
本项目中，服务器主循环为每一个连接创建一个定时器，并对每个连接进行定时。另用升序时间链表容器将所有定时器串联起来，若主循环接收到定时通知，则在链表中依次执行定时任务。

Linux下提供了三种定时的方法:

1. socket选项SO_RECVTIMEO和SO_SNDTIMEO
2. SIGALRM信号
3. I/O复用系统调用的超时参数

三种方法各有优劣。项目中使用的是SIGALRM信号，另外两种方法可以查阅《Linux高性能服务器编程》。

具体的，利用alarm函数周期性地触发SIGALRM信号，信号处理函数利用管道通知主循环，主循环接收到该信号后对升序链表上所有定时器进行处理，若该段时间内没有交换数据，则将该连接关闭，释放所占用的资源。

从上面的简要描述中，可以看出定时器处理非活动连接模块，主要分为两部分，其一为定时方法与信号通知流程，其二为定时器及其容器设计与定时任务的处理。

## 信号解析
### 一、核心结构体解析
结构体是信号机制的 “数据载体”，用于定义信号集、处理规则、详细信息等。
#### 1. sigset_t：信号集（Signal Set）
- 用途：表示一组信号的集合，是操作 “信号屏蔽、等待、关联” 的基础（如屏蔽哪些信号、等待哪些信号）。
- 本质：内部是一个位图（bitmask），每个 bit 对应一个信号（信号编号 1~64，需至少 64 位存储），但具体实现隐藏，必须通过专用函数操作（不可直接位运算）。
- 关键特性：
  - 不透明性：用户无需关心内部结构，仅通过sigemptyset/sigaddset等函数操作。
  - 不可屏蔽信号：SIGKILL（9）和SIGSTOP（19）无法加入信号集（系统强制生效，不能屏蔽 / 捕获）。
#### 2. struct sigaction：信号处理规则
- 用途：更灵活地定义信号的处理方式（如处理函数、屏蔽集、标志位），是信号处理的核心结构体。
- 结构体定义：
```c
struct sigaction {
    // 1. 普通处理函数（二选一，与sa_sigaction互斥）
    void (*sa_handler)(int);  // 参数为信号编号，或取值SIG_IGN（忽略）、SIG_DFL（默认）
    
    // 2. 带详细信息的处理函数（需设置SA_SIGINFO标志）
    void (*sa_sigaction)(int, siginfo_t *, void *);  // 能获取信号来源、数据等
    
    // 3. 处理函数执行期间，额外屏蔽的信号集
    sigset_t sa_mask;  // 系统会自动屏蔽当前信号，再叠加此集合（避免嵌套处理）
    
    // 4. 信号处理的控制标志（关键）
    int sa_flags;      // 如SA_RESTART（重启系统调用）、SA_SIGINFO（启用sa_sigaction）
    
    // 5. 已废弃（兼容旧系统，无需设置）
    void (*sa_restorer)(void);
};
```
- 核心成员解析：
  - sa_handler：简单处理函数，仅接收信号编号；若设为SIG_IGN，信号被忽略；SIG_DFL则执行系统默认行为（如终止进程）。
  - sa_sigaction：高级处理函数，需配合sa_flags=SA_SIGINFO，可获取sigaction_t（信号详细信息）和context（上下文）。
  - sa_mask：执行处理函数时，除了当前信号会被自动屏蔽，还会屏蔽此集合中的信号（避免同一信号嵌套触发，除非设SA_NODEFER）。
  - sa_flags：关键标志，常用值包括：
    - SA_RESTART：被信号中断的系统调用（如read、write、recv、send、accept等）自动重启，避免返回-1（errno==EINTR），而须手动处理中断；
    - SA_SIGINFO：启用sa_sigaction而非sa_handler；
    - SA_NODEFER：处理信号时不屏蔽当前信号（慎用，可能递归）；
    - SA_RESETHAND：处理后恢复为默认行为（只生效一次）。
    - SA_NOCLDSTOP，使父进程在它的子进程暂停或继续运行时不会收到 SIGCHLD 信号
    - SA_NOCLDWAIT，使父进程在它的子进程退出时不会收到 SIGCHLD 信号，这时子进程如果退出也不会成为僵尸进程
#### 3. struct siginfo_t：信号详细信息
- 用途：当使用sa_sigaction作为处理函数时，用于传递信号的 “上下文信息”（如发送者 PID、信号原因、携带数据）。
- 结构体完整定义（简化版，不同系统可能有细节差异，核心成员一致）：
```c
typedef struct siginfo_t {
    int si_signo;               // 信号编号（所有信号都包含）
    int si_errno;               // 错误码（部分信号携带，如异步I/O相关）
    int si_code;                // 信号产生的原因码（区分来源）
    pid_t si_pid;               // 发送信号的进程PID（用户发送的信号有效）
    uid_t si_uid;               // 发送信号的进程真实UID（用户发送的信号有效）
    void *si_addr;              // 内存错误相关地址（如SIGSEGV/SIGBUS时，指向出错地址）
    int si_status;              // 子进程状态（SIGCHLD时，存储退出状态或终止信号）
    long si_band;               // I/O事件相关（SIGIO时，指示就绪的I/O事件）
    union sigval si_value;      // 信号携带的数据（sigqueue发送的信号有效）
    // 以下为扩展成员，视信号类型而定
    timer_t si_timerid;         // 定时器ID（SIGALRM等定时器信号有效）
    int si_overrun;             // 定时器溢出次数（定时器信号有效）
    struct siginfo_t *si_ptr;   // 指向额外信息（部分信号使用）
} siginfo_t;

// 信号携带数据的联合体
union sigval {
    int sival_int;              // 整数数据
    void *sival_ptr;            // 指针数据（跨进程传递时需注意有效性）
};
```
- 核心成员解析：
  - si_signo：信号编号，所有信号都必须包含的基础信息。
  - si_code：信号产生的原因码，用于区分信号来源，例如：
      - SI_USER：由用户通过kill或sigqueue发送；
      - SI_KERNEL：由内核发送（如定时器到期、内存错误）；
      - SI_QUEUE：通过sigqueue发送且携带数据；
      - SI_CHLD：子进程状态变化触发（SIGCHLD 信号）。
  - si_pid和si_uid：仅当信号由其他进程发送（如kill/sigqueue）时有效，分别表示发送进程的 PID 和真实 UID，可用于权限校验。
  - si_addr：在内存访问错误相关信号（如SIGSEGV段错误、SIGBUS总线错误）中有效，指向触发错误的内存地址，便于调试。
  - si_status：仅SIGCHLD信号有效，存储子进程的退出状态（如WEXITSTATUS(si_status)获取退出码）或终止信号（如WTERMSIG(si_status)获取终止信号）。
  - si_value：由union sigval定义，仅当信号通过sigqueue发送时有效，用于传递整数（sival_int）或指针（sival_ptr）数据，实现进程间带数据的信号通信。
#### 4. struct sigevent：异步通知配置
- 用途：配合timer_create（定时器）、mq_notify（消息队列）等函数，定义 “异步事件触发时的通知方式”（如发信号、创建线程）。
- 结构体定义与核心成员：
```c
struct sigevent {
    int sigev_notify;  // 通知方式
    int sigev_signo;   // 通知信号（仅sigev_notify=SIGEV_SIGNAL时有效）
    union sigval sigev_value;  // 传递的数据（信号或线程回调可用）
    // 线程回调相关（sigev_notify=SIGEV_THREAD时有效）
    void (*sigev_notify_function)(union sigval);  // 线程回调函数
    pthread_attr_t *sigev_notify_attributes;      // 新线程属性（如栈大小）
};
```
- 关键成员解析：
  - sigev_notify：指定事件触发时的通知方式，常用值包括：
      - SIGEV_SIGNAL：事件触发时，向进程发送sigev_signo指定的信号；
      - SIGEV_THREAD：事件触发时，自动创建线程执行sigev_notify_function；
      - SIGEV_NONE：无通知（仅用于查询事件状态）。
  - sigev_value：传递给信号处理函数（通过si_value）或线程回调函数的参数，支持整数或指针类型。

### 二、关键接口解析
接口是操作信号的 “工具函数”，按功能分为信号集操作、处理注册、发送、阻塞、等待五大类。
#### 一、信号集操作接口（操作sigset_t）
因sigset_t是不透明结构体，必须通过以下函数初始化、添加 / 删除信号。
- int sigemptyset(sigset_t *set)：初始化信号集set，清空所有信号（bit 全 0）。成功返回 0，失败返回 - 1（设置 errno）。
- int sigfillset(sigset_t *set)：初始化信号集set，包含所有已定义信号（bit 全 1）。成功返回 0，失败返回 - 1（设置 errno）。
- int sigaddset(sigset_t *set, int sig)：将信号sig添加到set中（bit 置 1）。成功返回 0，失败返回 - 1（设置 errno）。
- int sigdelset(sigset_t *set, int sig)：从set中删除信号sig（bit 置 0）。成功返回 0，失败返回 - 1（设置 errno）。
- int sigismember(const sigset_t *set, int sig)：检查sig是否在set中。存在返回 1，不存在返回 0，失败返回 - 1（设置 errno）。
注意：使用sigset_t前必须调用sigemptyset或sigfillset初始化，否则内容未定义。
#### 二、信号处理注册接口（设置信号处理方式）
用于指定 “进程收到信号后该做什么”，优先使用sigaction（signal函数可移植性差）。
##### 1. signal：简单注册（不推荐）
- 原型：
 ```c 
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t handler);
```
- 功能：为信号sig注册处理函数handler（或SIG_IGN/SIG_DFL）。
- 缺点：
    - 可移植性差：不同系统对 “是否重启系统调用”“是否自动恢复默认行为” 的实现不一致；
    - 无法获取信号详细信息（仅能接收信号编号）。
返回值：成功返回旧处理函数，失败返回SIG_ERR（设置 errno）。
##### 2. sigaction：高级注册（推荐）
- 原型：
```c
- int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
```
- 功能：为信号sig设置处理规则（act），并可获取旧规则（oldact）。
- 参数：
    - sig：目标信号（不能是SIGKILL/SIGSTOP）；
    - act：新处理规则（NULL表示仅获取旧规则）；
    - oldact：存储旧规则（NULL表示不获取）。
- 返回值：成功返回 0，失败返回 - 1（设置 errno）。
- 示例：注册SIGINT（Ctrl+C）的高级处理函数，获取发送者 PID：
```c
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

void sigint_handler(int sig, siginfo_t *info, void *ctx) {
    printf("收到SIGINT，发送者PID：%d，UID：%d\n", info->si_pid, info->si_uid);
}

int main() {
    struct sigaction act;
    sigemptyset(&act.sa_mask);    // 处理期间不额外屏蔽信号
    act.sa_flags = SA_SIGINFO;    // 启用sa_sigaction
    act.sa_sigaction = sigint_handler;

    if (sigaction(SIGINT, &act, NULL) == -1) {
        perror("sigaction failed");
        return 1;
    }

    while (1) sleep(1);  // 等待信号
    return 0;
}
```
#### 三、信号发送接口（向进程 / 线程发信号）
主动触发信号，支持进程间、线程间通信。
- int kill(pid_t pid, int sig)：向进程pid发送信号sig。pid的取值规则：pid>0指向指定进程；pid=0指向同进程组所有进程；pid=-1指向有权限的所有进程；pid<-1指向进程组-pid的所有进程。
- int raise(int sig)：向当前进程发送信号sig（等价于kill(getpid(), sig)）。无进程 ID 参数，仅作用于自身；返回值非 0 表示失败（不设 errno，可移植性差）。
- int pthread_kill(pthread_t tid, int sig)：向当前进程的线程tid发送信号sig。tid是pthread_t类型（线程 ID，非系统 TID）；sig不能是SIGKILL/SIGSTOP（会终止整个进程）。
- int sigqueue(pid_t pid, int sig, const union sigval val)：向进程pid发送信号sig，并携带数据val。union sigval支持int或void*（跨进程传递指针无效）；需配合SA_SIGINFO处理函数获取数据。
- **权限**：发送者有效 UID 需等于接收者有效 / 真实 UID，或具有root权限，否则返回EPERM。
#### 四、信号阻塞与挂起接口（控制信号递送时机）
信号阻塞（屏蔽）：被阻塞的信号不会立即递送，会暂存为 “未决状态”，直到阻塞解除。
##### 1. sigprocmask/pthread_sigmask：修改信号屏蔽字
- 原型：
```c
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
```
- 功能：修改当前进程的信号屏蔽字（阻塞集），pthread_sigmask作用于当前线程（多线程推荐用）。
- 关键参数：
    - how：修改方式：SIG_BLOCK（添加set到屏蔽字）、SIG_UNBLOCK（从屏蔽字中删除set）、SIG_SETMASK（用set替换屏蔽字）；
    - set：要操作的信号集（NULL表示仅获取旧屏蔽字）；
    - oldset：存储旧屏蔽字（NULL表示不获取）。
- 返回值：sigprocmask成功返回 0，失败返回 - 1（设置 errno）；pthread_sigmask失败返回错误码（非 - 1）。
##### 2. sigpending：获取未决信号集
- 原型：int sigpending(sigset_t *set);
- 功能：将当前进程中 “已发送但被阻塞” 的信号（未决信号）存入set。
- 用途：调试时检查信号是否被正确阻塞（如确认SIGINT是否因屏蔽而未递送）。
##### 3. sigsuspend：原子性挂起等待信号
- 原型：int sigsuspend(const sigset_t *mask);
- 功能：临时将屏蔽字替换为mask，然后挂起进程，直到收到 “未被mask阻塞” 的信号（处理后返回）。
- 关键特性：原子操作（替换屏蔽字 + 挂起），避免 “检查信号标志→挂起” 的竞态条件。
- 返回值：始终返回 - 1，errno 设为EINTR（仅被信号中断后返回）。
- 示例：安全等待SIGINT，避免竞态：
```c
sigset_t mask, old_mask;
sigemptyset(&mask);                // 临时屏蔽字：不阻塞任何信号
sigaddset(&old_mask, SIGINT);      // 旧屏蔽字：阻塞SIGINT
sigprocmask(SIG_BLOCK, &old_mask, NULL);  // 先阻塞SIGINT

// 执行逻辑后，等待SIGINT（原子替换屏蔽字+挂起）
sigsuspend(&mask);  // 临时解除SIGINT阻塞，收到信号后恢复旧屏蔽字
```
#### 五、信号等待接口（主动等待信号）
无需注册处理函数，主动阻塞等待信号，适合 “集中处理信号” 的场景（如多线程中用一个线程处理所有信号）。
- int sigwaitinfo(const sigset_t *set, siginfo_t *info)：阻塞等待set中的信号，获取详细信息（存入info）。无超时，直到收到信号才返回；set中的信号必须已被屏蔽（否则信号可能提前递送）。
- int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout)：同sigwaitinfo，但支持超时。timeout指定超时时间（秒 + 纳秒）；超时返回 - 1，errno=EAGAIN。
返回值：成功返回信号编号，失败返回 - 1（设置 errno）。
## 三、核心注意事项
- **不可捕获 / 屏蔽的信号**：
    - SIGKILL（9）和SIGSTOP（19）是系统强制信号，无法通过sigaction注册处理函数，也无法通过sigprocmask屏蔽。
- **异步信号安全函数**：
    - 信号处理函数是异步执行的，只能调用异步安全函数（如_exit、write、sigprocmask），严禁调用非安全函数（如printf、malloc、exit）—— 会导致全局数据结构损坏（如printf缓冲区崩溃、malloc堆混乱）。
    - 常用安全函数列表：man 7 signal的Async-Signal-Safe Functions章节。
- **多线程信号处理**：
    - 信号处理函数是进程级共享的（一个线程注册，所有线程生效）；
    - 线程有独立的信号屏蔽字（用pthread_sigmask修改）；
    - 建议：让一个 “信号处理线程” 通过sigwaitinfo集中处理信号，其他线程用pthread_sigmask屏蔽所有信号，避免异步处理带来的竞态。
- **实时信号与普通信号**：
    - 普通信号（1~31）：不支持排队，多次发送同一信号若被阻塞，仅递送一次；
    - 实时信号（32~64，SIGRTMIN~SIGRTMAX）：支持排队，多次发送会依次递送，优先级高于普通信号。

## 信号通知流程
- Linux下的信号采用的异步处理机制，信号处理函数和当前进程是两条不同的执行路线。
- 具体的，当进程收到信号时，操作系统会中断进程当前的正常流程，转而进入信号处理函数执行操作，完成后再返回中断的地方继续执行。
- 为避免信号竞态现象发生，信号处理期间系统不会再次触发它。所以，为确保该信号不被屏蔽太久，信号处理函数需要尽可能快地执行完毕。
- 一般的信号处理函数需要处理该信号对应的逻辑，当该逻辑比较复杂时，信号处理函数执行时间过长，会导致信号屏蔽太久。
- 此处解决方案是，信号处理函数仅仅发送信号通知程序主循环，将信号对应的处理逻辑放在程序主循环中，由主循环执行信号对应的逻辑代码。

### 统一事件源
统一事件源，是指将信号事件与其他事件一样被处理。

具体的，信号处理函数使用管道将信号传递给主循环，信号处理函数往管道的写端写入信号值，主循环则从管道的读端读出信号值，使用I/O复用系统调用来监听管道读端的可读事件，这样信号事件与其他文件描述符都可以通过epoll来监测，从而实现统一处理。
### 信号通知逻辑(socketpair)
- 创建管道，其中管道写端写入信号值，管道读端通过I/O复用系统监测读事件
- 设置信号处理函数SIGALRM（时间到了触发）和SIGTERM（kill会触发，Ctrl+C）
   - 通过struct sigaction结构体和sigaction函数注册信号捕捉函数
   - 在结构体的handler参数设置信号处理函数，具体的，从管道写端写入信号的名字
- 利用I/O复用系统监听管道读端文件描述符的可读事件
- 信息值传递给主循环，主循环再根据接收到的信号值执行目标信号对应的逻辑代码

### 信号处理机制
每个进程之中，都有存着一个表，里面存着每种信号所代表的含义，内核通过设置表项中每一个位来标识对应的信号类型。
![信号处理机制](https://mmbiz.qpic.cn/mmbiz_jpg/6OkibcrXVmBF4pFdWIo9AHPnib7HCeX9t4u3DhF2ywtNlamuVEDmd0IGDI3klPTJpPvjvric8U490RvzueCe7icTOg/640?wx_fmt=jpeg&tp=webp&wxfrom=5&wx_lazy=1#imgIndex=1)

- 信号的接收

    - 接收信号的任务是由内核代理的，当内核接收到信号后，会将其放到对应进程的信号队列中，同时向进程发送一个中断，使其陷入内核态。注意，此时信号还只是在队列中，对进程来说暂时是不知道有信号到来的。

- 信号的检测
  
    - 进程陷入内核态后，有两种场景会对信号进行检测：

    > - 进程从内核态返回到用户态前进行信号检测
    > - 进程在内核态中，从睡眠状态被唤醒的时候进行信号检测

    - 当发现有新信号时，便会进入下一步，信号的处理。

- 信号的处理

    - ( **内核** )信号处理函数是运行在用户态的，调用处理函数前，内核会将当前内核栈的内容备份拷贝到用户栈上，并且修改指令寄存器（eip）将其指向信号处理函数。

    - ( **用户** )接下来进程返回到用户态中，执行相应的信号处理函数。

    - ( **内核** )信号处理函数执行完成后，还需要返回内核态，检查是否还有其它信号未处理。

    - ( **用户** )如果所有信号都处理完成，就会将内核栈恢复（从用户栈的备份拷贝回来），同时恢复指令寄存器（eip）将其指向中断前的运行位置，最后回到用户态继续执行进程。

至此，一个完整的信号处理流程便结束了，如果同时有多个信号到达，上面的处理流程会在第2步和第3步骤间重复进行。


## 三种时钟类型总览
Linux 系统通过 setitimer/getitimer 接口支持的三种时钟类型如下：  

|时钟类型|计时范围（计数对象）|触发信号|核心用途|
|:---|:---|:---|:---|
|ITIMER_REAL|系统真实时间（墙上时间）|SIGALRM|真实时间定时（如闹钟、超时）|
|ITIMER_VIRTUAL|进程在用户态消耗的 CPU 时间|SIGVTALRM|限制用户态执行时间|
|ITIMER_PROF|进程在用户态 + 内核态消耗的总 CPU 时间|	SIGPROF|性能分析（总 CPU 消耗统计）|
## SIGALRM 信号：核心结构与编程接口解析
SIGALRM 是 Linux/Unix 系统中的 “闹钟信号”，主要用于通知进程 “某个定时器已到期”。它通常与系统定时器（如 ITIMER_REAL）绑定，当定时器倒计时结束时，内核会向目标进程发送 SIGALRM 信号。若进程未主动处理该信号，默认行为是 终止进程。
### 一、核心概念

在解析结构和接口前，需明确两个关键关联：
- 时钟类型：
  - SIGALRM 仅与 ITIMER_REAL 时钟绑定（该时钟基于系统真实时间，倒计时期间即使进程休眠也会继续）；
  - 其他时钟（如 ITIMER_VIRTUAL 对应 SIGVTALRM）不触发 SIGALRM。
- 信号特性：
    - SIGALRM 是 **不可靠信号**（信号可能丢失，若前一个 SIGALRM 未处理，后一个会被丢弃），且**默认处理行为是终止进程**，因此实际编程中必须主动注册信号处理函数。
### 二、相关数据结构
与 SIGALRM 编程直接相关的核心结构有两个：itimerval（定义定时器时间参数）和 sigaction（定义信号处理规则---如前述）。
#### itimerval：定时器时间结构
- 用于描述定时器的 “初始延迟时间” 和 “周期重复时间”，是 setitimer/getitimer 接口的核心参数。
- 结构定义
```c
#include <sys/time.h>

struct itimerval {
    struct timeval it_interval;  // 周期重复时间（定时器触发后，下次触发的间隔）
    struct timeval it_value;     // 初始延迟时间（定时器启动后，首次触发的时间）
};

// 辅助结构：描述具体时间（秒+微秒）
struct timeval {
    time_t      tv_sec;  // 秒（seconds）
    suseconds_t tv_usec; // 微秒（microseconds，范围 0-999999）
};
```
- 关键说明
  - it_value：决定 “定时器何时首次触发”。若 it_value 为 (0, 0)，表示定时器未激活（即使 it_interval 非零也无效）。
  - it_interval：决定 “定时器是否周期性触发”。
    - 若 it_interval 为 (0, 0)：定时器是一次性的（仅触发一次后自动停止）。
    - 若 it_interval 非零：定时器首次触发后，会自动以 it_interval 为间隔重复触发，直到手动取消。
### 三、核心编程接口
与 SIGALRM 相关的接口主要分为两类：定时器控制接口（设置 / 获取 / 取消定时器）和 信号处理接口（注册 / 修改信号处理规则---如前述）。
#### 1. 定时器控制接口
##### （1）setitimer：设置 / 修改 / 取消定时器
- 功能：配置 ITIMER_REAL 时钟（对应 SIGALRM）的参数，实现一次性或周期性定时器。
- 函数原型
```c
#include <sys/time.h>

int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value);
```
- 参数说明
  
|参数|含义|
|:---|:---|
|which|时钟类型，必须为 ITIMER_REAL（唯一触发 SIGALRM 的时钟）。|
|new_value|指向新的定时器参数（itimerval 结构），用于设置初始延迟和周期。|
|old_value|可选（可为 NULL），用于保存之前的定时器状态（便于后续恢复）。|

- 返回值
    - 成功：返回 0。
    - 失败：返回 -1，并设置 errno（如 EINVAL：which 无效或时间参数非法）。
- 关键用法
    -  启动一次性定时器：it_value 设为目标延迟（如 3 秒），it_interval 设为 (0, 0)。
    - 启动周期性定时器：it_value 设为首次延迟（如 1 秒），it_interval 设为周期（如 2 秒）。
    - 取消定时器：new_value 的 it_value 和 it_interval 均设为 (0, 0)。
##### （2）getitimer：获取当前定时器状态
- 功能：查询 ITIMER_REAL 时钟的当前剩余时间和周期参数。
- 函数原型
```c
#include <sys/time.h>

int getitimer(int which, struct itimerval *curr_value);
```
- 参数解析
    - which：时钟类型，必须为 ITIMER_REAL。
    - curr_value：指向 itimerval 结构，用于存储当前定时器状态（it_value 为剩余时间，it_interval 为周期）。
- 返回值
    - 成功：返回 0。
    - 失败：返回 -1，并设置 errno。
##### （2）alarm：简化版定时器接口（仅一次性）
- 功能：设置一个一次性的秒级定时器，本质是 setitimer(ITIMER_REAL, ...) 的简化封装，仅支持秒级精度，且无法设置周期。
- 函数原型
```c
#include <unistd.h>

unsigned int alarm(unsigned int seconds);
```
- 参数与返回值
    - seconds：定时器延迟（秒），若为 0，则取消之前的 alarm 定时器。
    - 返回值：之前未到期的 alarm 剩余秒数（若之前无 alarm，返回 0）。
- 示例：3 秒后触发 SIGALRM
```c
alarm(3); // 3 秒后内核发送 SIGALRM
pause();  // 阻塞等待信号
```
## 其他
### EPOLLONESHOT 
在 Linux 的 epoll 机制中，EPOLLONESHOT 是一个事件标志位，其核心作用是**确保某个文件描述符（fd）的事件（如可读、可写）被触发一次后，就会被从 epoll 的就绪列表中移除，直到再次通过 epoll_ctl 重新注册该事件**。
- 具体作用与场景
在多线程 / 多进程处理 I/O 事件的场景中，EPOLLONESHOT 主要用于解决事件重复触发导致的并发冲突问题。
例如：当一个 socket 描述符上有数据可读时，epoll 会将其加入就绪列表并通知应用程序。如果没有 EPOLLONESHOT，若该 socket 上的数据未被一次性处理完（或处理过程中又有新数据到达），epoll 可能会再次触发可读事件，导致多个线程同时处理同一个 socket 的数据，引发数据错乱（如重复读取、读取不完整等）。
  - 而使用 EPOLLONESHOT 后：
    1. 事件触发一次后，该文件描述符会被自动 “禁用”（不再触发新事件）；
    2. 只有当负责处理的线程完成数据处理后，主动通过 epoll_ctl 重新注册该文件描述符的事件（再次带上 EPOLLONESHOT），才能继续监听后续事件。
- 总结
EPOLLONESHOT 的核心价值是保证一个 ** I/O 事件在多线程环境下只会被一个线程处理**，避免并发处理同一文件描述符时的资源竞争问题，尤其适合需要 “独占式” 处理 I/O 事件的场景。
