
http连接处理类
===============
根据状态转移,通过主从状态机封装了http连接类。其中,主状态机在内部调用从状态机,从状态机将处理状态和数据传给主状态机
> * 客户端发出http连接请求
> * 从状态机读取数据,更新自身状态和接收数据,传给主状态机
> * 主状态机根据从状态机状态,更新自身状态,决定响应请求还是继续读取

## select/poll/epoll
- 调用函数

    - select和poll都是一个函数，epoll是一组函数

- 文件描述符数量

    - select通过线性表描述文件描述符集合，文件描述符有上限，一般是1024，但可以修改源码，重新编译内核，不推荐

    - poll是链表描述，突破了文件描述符上限，最大可以打开文件的数目

    - epoll通过红黑树描述，最大可以打开文件的数目，可以通过命令ulimit -n number修改，仅对当前终端有效

- 将文件描述符从用户传给内核

    - select和poll通过将所有文件描述符拷贝到内核态，每次调用都需要拷贝

    - epoll通过epoll_create建立一棵红黑树，通过epoll_ctl将要监听的文件描述符注册到红黑树上

- 内核判断就绪的文件描述符

    - select和poll通过遍历文件描述符集合，判断哪个文件描述符上有事件发生

    - epoll_create时，内核除了帮我们在epoll文件系统里建了个红黑树用于存储以后epoll_ctl传来的fd外，还会再建立一个list链表，用于存储准备就绪的事件，当epoll_wait调用时，仅仅观察这个list链表里有没有数据即可。

    - epoll是根据每个fd上面的回调函数(中断函数)判断，只有发生了事件的socket才会主动的去调用 callback函数，其他空闲状态socket则不会，若是就绪事件，插入list

- 应用程序索引就绪文件描述符

    - select/poll只返回发生了事件的文件描述符的个数，若知道是哪个发生了事件，同样需要遍历

    - epoll返回的发生了事件的个数和结构体数组，结构体包含socket的信息，因此直接处理返回的数组即可

- 工作模式

    - select和poll都只能工作在相对低效的LT模式下

    - epoll则可以工作在ET高效模式，并且epoll还支持EPOLLONESHOT事件，该事件能进一步减少可读、可写和异常事件被触发的次数。 

- 应用场景

    - 当所有的fd都是活跃连接，使用epoll，需要建立文件系统，红黑书和链表对于此来说，效率反而不高，不如selece和poll

    - 当监测的fd数目较小，且各个fd都比较活跃，建议使用select或者poll

    - 当监测的fd数目非常大，成千上万，且单位时间只有其中的一部分fd处于就绪状态，这个时候使用epoll能够明显提升性能

## ET、LT、EPOLLONESHOT
- LT水平触发模式
    - 事件就绪而未完全处理，epoll_wait会一直通知应用程序。

    - epoll_wait检测到文件描述符有事件发生，则将其通知给应用程序，应用程序可以完全处理/部分处理/不处理该事件。

    - 当下一次调用epoll_wait时，epoll_wait还会再次向应用程序报告此事件，直至被完全处理

- ET边缘触发模式
    - 事件就绪仅通知一次，无关处理进度如何，故通常需配合非阻塞 I/O一次性处理完所有数据。

    - epoll_wait检测到文件描述符有事件发生，则将其通知给应用程序，应用程序必须立即处理该事件

    - 必须要一次性将数据读取完，使用非阻塞I/O，读取到出现eagain

- EPOLLONESHOT
  
    - 一个线程读取某个socket上的数据后开始处理数据，在处理过程中该socket上又有新数据可读，此时另一个线程被唤醒读取，此时出现两个线程处理同一个socket

    - 我们期望的是一个socket连接在任一时刻都只被一个线程处理，通过epoll_ctl对该文件描述符注册epolloneshot事件，一个线程处理socket时，其他线程将无法处理，当该线程处理完后，需要通过epoll_ctl重置epolloneshot事件

## http报文
HTTP 报文是客户端（如浏览器）与服务器间基于 HTTP 协议传输数据的 “格式规范”，分为请求报文（客户端→服务器）和响应报文（服务器→客户端）两类。二者核心结构一致，均由「起始行 + 首部字段 + 空行 + 消息体」四部分组成 —— 空行是关键分隔符，用于区分首部与消息体，即便没有消息体，空行也不能省略，否则会导致解析失败。
### 一、HTTP 报文整体结构
无论请求还是响应，报文都遵循以下固定范式：
```
[起始行]  # 核心指令，描述请求目的或响应结果
[首部字段]  # 键值对格式（字段名: 值），补充元数据（如数据类型、缓存策略）
[空行]      # 强制存在，标志首部结束、消息体开始
[消息体]    # 可选，实际传输的数据（如表单、HTML、JSON）
```
HTTP/1.1 规范强制要求：所有 “行级分割” 必须用 \r\n 作为分隔符
空行：仅包含 \r\n

### 二、请求报文解析（客户端→服务器）
请求报文的作用是 “告诉服务器：要操作的资源、操作方式，以及附加需求”，核心在于请求行和请求相关首部。
#### 1. 核心：请求行（起始行）
请求行是报文第一行，由「请求方法 + 请求 URI + HTTP 版本」三部分用空格分隔组成,格式为：
```
<请求方法> <请求URI> <HTTP版本>
```
- 请求方法：定义对服务器资源的操作类型，HTTP 1.1 常用 8 种，核心几种如下：
    > - GET：用于获取资源（如打开网页、查询数据），参数通常拼在 URI 后（例：/user?id=1），消息体可选，且是 “幂等” 的（多次请求结果一致，不会改变服务器状态）。
    > - POST：用于提交资源（如表单提交、上传文件），参数放在消息体中（更安全、支持大数据），非幂等（多次提交可能产生不同结果，如重复下单）。
    > - PUT：用于全量更新资源（如修改用户信息），幂等。
    > - DELETE：用于删除资源，幂等。
    > - HEAD：仅获取响应首部（不返回消息体），常用于检查资源是否存在、获取文件大小。
- 请求 URI：指定服务器上的目标资源路径，可是绝对路径（例：/index.html）或完整 URL（例：https://www.example.com/api/user）。
- HTTP 版本：声明使用的协议版本，主流为 HTTP/1.1，HTTP/2、HTTP/3 因效率更高逐步普及。
- 请求行示例：GET /index.html HTTP/1.1、POST /api/login HTTP/1.1。
#### 2. 请求首部字段
首部字段是**键值对**，按功能分为通用首部（请求 / 响应通用）、请求首部（仅请求用）、实体首部（描述消息体）三类，核心字段如下：
- 通用首部：
Connection：声明连接模式，keep-alive 表示复用当前连接（减少连接建立开销），close 表示请求完成后关闭连接，示例：Connection: keep-alive。
- 请求首部：
    > - Host：HTTP/1.1 必须包含的字段，指定服务器的域名和端口（用于虚拟主机区分多个网站，比如同一服务器上的 www.baidu.com 和 map.baidu.com），示例：Host: www.example.com。
    > - User-Agent：告诉服务器客户端 “身份”，包括浏览器版本、操作系统、爬虫标识等（服务器可据此返回适配内容），示例：User-Agent: Chrome/118.0.0.0。
    > - Accept：声明客户端可接受的响应数据格式（MIME 类型），服务器会优先返回匹配格式，示例：Accept: text/html, application/json（表示接受 HTML 页面或 JSON 数据）。
    > - Cookie：携带客户端本地存储的 Cookie（用于身份认证、会话保持，比如登录后服务器下发的 sessionid），示例：Cookie: sessionid=abc123; username=admin。
- 实体首部：仅当请求有消息体时需要，描述消息体的属性：
    > - Content-Type：声明消息体的数据格式，服务器据此解析数据，示例：Content-Type: application/json（消息体是 JSON）、Content-Type: application/x-www-form-urlencoded（消息体是表单数据）。
    > - Content-Length：声明消息体的字节大小（帮助服务器准确接收数据，避免漏读或多读），示例：Content-Length: 43（表示消息体共 43 个字节）。
#### 3. 请求消息体
**可选**，仅在需要向服务器提交数据时存在（如 POST/PUT 请求），常见格式有三种：
- 表单数据：格式为 username=admin&password=123，对应 Content-Type: application/x-www-form-urlencoded。
- JSON 数据：格式为 {"username":"admin","password":"123"}，对应 Content-Type: application/json。
- 文件流：二进制数据（如图片、文档），对应 Content-Type: multipart/form-data（上传文件时常用）。
### 4. 完整请求报文示例（POST 请求）
```
GET /562f25980001b1b106000338.jpg HTTP/1.1
Host:img.mukewang.com
User-Agent:Mozilla/5.0 (Windows NT 10.0; WOW64)
AppleWebKit/537.36 (KHTML, like Gecko) Chrome/51.0.2704.106 Safari/537.36
Accept:image/webp,image/*,*/*;q=0.8
Referer:http://www.imooc.com/
Accept-Encoding:gzip, deflate, sdch
Accept-Language:zh-CN,zh;q=0.8
空行
请求数据为空
```

```
POST /api/login HTTP/1.1  # 请求行：方法=POST，URI=/api/login，版本=HTTP/1.1
Host: www.example.com     # 请求首部：目标服务器域名
User-Agent: Chrome/118.0.0.0
Accept: application/json  # 接受JSON格式的响应
Content-Type: application/json  # 消息体是JSON格式
Content-Length: 43        # 消息体大小43字节
Connection: keep-alive
Cookie: sessionid=old123  # 携带旧会话ID

{"username":"admin","password":"123456"}  # 消息体：登录数据
```
### 三、响应报文解析（服务器→客户端）
响应报文的作用是 “告诉客户端：请求处理结果，以及返回的数据”，核心在于状态行和响应相关首部。
#### 1. 核心：状态行（起始行）
状态行是报文第一行，由「HTTP 版本 + 状态码 + 原因短语」三部分用**空格**分隔组成，格式为：
```
<HTTP版本> <状态码> <原因短语>
```
- HTTP 版本：与请求报文一致，例：HTTP/1.1。
- 状态码：3 位数字，标识请求的处理结果，按首位数字分为 5 类，是问题定位的核心依据：
    > - 1xx（信息提示）：服务器已接收请求，需客户端继续操作（实际开发中极少用），例：100 Continue（表示客户端可继续发送消息体）。
    > - 2xx（成功）：请求被服务器正常处理，例：200 OK（请求成功且返回消息体）、204 No Content（请求成功但无消息体，如删除资源后无数据返回）。
    > - 3xx（重定向）：客户端需进一步操作才能获取资源，例：302 临时重定向（资源临时迁移到新地址）、304 Not Modified（资源未修改，客户端可直接用本地缓存）。
    > - 4xx（客户端错误）：请求本身有问题（如地址错、权限不足），服务器无法处理，例：404 Not Found（资源不存在，常因 URI 写错）、403 Forbidden（权限不足，如未登录访问需要权限的页面）、400 Bad Request（请求参数错误）。
    > - 5xx（服务器错误）：客户端请求无问题，但服务器处理时出错，例：500 Internal Server Error（服务器代码报错）、503 Service Unavailable（服务器过载或维护，暂时无法服务）。
- 原因短语：对状态码的文字描述（仅为可读性，核心还是状态码），例：OK（对应 200）、Not Found（对应 404）。
- 状态行示例：HTTP/1.1 200 OK、HTTP/1.1 404 Not Found。
#### 2. 响应首部字段
同样按功能分为通用首部、响应首部、实体首部三类，核心字段如下：
- 通用首部：与请求报文一致，如 Connection: keep-alive、Date: Wed, 11 Oct 2024 09:00:00 GMT（声明响应生成时间）。
- 响应首部：仅响应报文用：
    > - Server：声明服务器的软件信息（如 Web 服务器类型），示例：Server: Nginx/1.24.0。
    > - Set-Cookie：服务器向客户端设置 Cookie（用于身份认证、会话跟踪，比如登录成功后下发新的 sessionid），示例：Set-Cookie: sessionid=new456; Path=/; HttpOnly（HttpOnly 防止 JS 读取，提升安全性）。
    > - Location：配合 3xx 重定向使用，指定资源的新地址，示例：Location: https://www.new-example.com（客户端会自动跳转到该地址）。
- 实体首部：描述响应消息体的属性：
    > - Content-Type：声明响应消息体的数据格式，客户端据此解析数据（如浏览器根据 text/html 渲染页面，前端根据 application/json 解析数据），示例：Content-Type: text/html; charset=utf-8（HTML 页面，编码为 UTF-8）。
    > - Content-Length：声明响应消息体的字节大小，示例：Content-Length: 1024（消息体共 1024 字节）。
    > - Cache-Control：控制资源的缓存策略，示例：Cache-Control: public, max-age=3600（表示资源可公开缓存，缓存有效期 1 小时）。
#### 3. 响应消息体
**可选**，是服务器返回给客户端的实际数据，常见格式：
- HTML 页面：<!DOCTYPE html><html><head><title>首页</title></head><body>Hello</body></html>，对应 Content-Type: text/html。
- JSON 数据：{"code":200,"message":"登录成功","data":{"username":"admin"}}，对应 Content-Type: application/json。
- - 静态资源：图片（Content-Type: image/jpeg）、CSS 文件（Content-Type: text/css）、JS 文件（Content-Type: application/javascript）等二进制或文本数据。
#### 4. 完整响应报文示例（200 成功响应）
```
HTTP/1.1 200 OK  # 状态行：版本=HTTP/1.1，状态码=200（成功），原因短语=OK
Server: Nginx/1.24.0  # 响应首部：服务器软件
Date: Wed, 11 Oct 2024 09:00:00 GMT  # 通用首部：响应时间
Content-Type: application/json; charset=utf-8  # 实体首部：返回JSON格式
Content-Length: 58  # 实体首部：消息体58字节
Set-Cookie: sessionid=new456; Path=/; HttpOnly  # 响应首部：设置新会话Cookie
Cache-Control: max-age=3600  # 通用首部：缓存1小时

{"code":200,"message":"登录成功","data":{"username":"admin","role":"admin"}}  # 消息体
```
### 四、报文解析关键注意点  

1. 空行不可省略：首部字段与消息体之间必须用「空行」（即 \r\n）分隔，否则服务器 / 客户端无法区分首部和消息体，会导致解析异常。
2. 首部字段大小写不敏感：比如 Host 和 host、User-Agent 和 user-agent 等价，但规范建议首字母大写（如 Content-Type），提升可读性。
3. 消息体可选：GET/HEAD 请求通常无消息体；204 No Content、304 Not Modified 等响应也无消息体（即便设置了 Content-Length，也会被忽略）。
4. HTTP/2 与 HTTP/1.1 的差异：HTTP/1.1 报文是文本格式，HTTP/2 转为二进制帧（Binary Framing）传输，但逻辑结构（起始行、首部、消息体）不变，只是解析方式从 “文本读取” 变为 “二进制解析”，效率更高。

### 五、实际解析工具
若需查看真实 HTTP 报文，可使用以下工具：
- 浏览器开发者工具：按 F12 打开 → 进入「Network」面板 → 点击任意请求 → 「Headers」标签查看首部、「Response」标签查看响应消息体、「Request」标签查看请求消息体。
- 命令行工具：执行 curl -v [URL]（-v 表示 “详细模式”），会输出完整的请求和响应报文。
- 抓包工具：Wireshark（底层抓包，可查看 TCP/IP + HTTP 完整报文）、Fiddler（代理抓包，适合接口调试，可修改报文内容）。

## 相关函数
### 1. 字符串比较函数
```c
#include <string.h>

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
```
这两个函数用于比较两个字符串，忽略大小写。它们分别比较整个字符串和前 n 个字符。如果两个字符串相等，则返回 0；如果第一个字符串小于第二个字符串，则返回一个负数；如果第一个字符串大于第二个字符串，则返回一个正数。
```c
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
```
这两个函数用于比较两个字符串，不忽略大小写。它们分别比较整个字符串和前 n 个字符。如果两个字符串相等，则返回 0；如果第一个字符串小于第二个字符串，则返回一个负数；如果第一个字符串大于第二个字符串，则返回一个正数。

### 2. 字符串查找函数
```c
char *strpbrk(const char *s, const char *accept);
```
strpbrk() 函数用于查找字符串中第一个在指定字符集中的字符的位置，并返回指向该字符的指针。如果字符串中所有字符都不在指定字符集中，则返回 NULL。

```c
char *strstr(const char *haystack, const char *needle);
```
strstr() 函数用于查找字符串中第一个与指定子字符串匹配的位置，并返回指向该位置的指针。如果字符串中不存在指定的子字符串，则返回 NULL。

### 3. 字符串分割函数
```c
#include <string.h>

char *strtok(char *str, const char *delim);
```
strtok() 函数用于将字符串分割成多个子字符串。它接受两个参数：要分割的字符串和分隔符字符集。函数会返回指向下一个子字符串的指针，并修改原始字符串，在每个分隔符处将其替换为空字符。如果字符串中没有更多的子字符串，则返回 NULL。

### 4. 字符串匹配函数
```c
#include <string.h>

// 计算s中连续包含于accept字符集的初始段长度
size_t strspn(const char *s, const char *accept);

// 计算s中连续不包含于reject字符集的初始段长度
size_t strcspn(const char *s, const char *reject);
```
这两个函数用于计算字符串中连续包含或排除指定字符集的初始段长度。它们分别返回字符串中第一个不在 accept 字符集中的字符的位置或第一个在 reject 字符集中的字符的位置。
