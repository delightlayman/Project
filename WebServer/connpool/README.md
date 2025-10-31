
校验 & 数据库连接池
===============
数据库连接池
> * 单例模式，保证唯一
> * list实现连接池
> * 连接池为静态大小
> * 互斥锁实现线程安全

校验  
> * HTTP请求采用POST方式
> * 登录用户名和密码校验
> * 用户注册及多线程注册安全

---------
# mysql常用C API及其相关结构解析
## 一、核心结构体解析
MySQL C API 中最常用的结构体用于描述连接状态、查询结果、数据行和字段信息，是操作数据库的基础。
### 1. MYSQL 结构体
**作用**：代表一个 MySQL 连接句柄，存储数据库连接的所有状态信息（如连接参数、服务器信息、错误状态等），是所有数据库操作的起点。
主要成员（简化）：
```c
typedef struct st_mysql {
char *host;        // 数据库主机名
char *user;        // 用户名
char *passwd;      // 密码
char *db;          // 当前数据库名
unsigned int port; // 端口号（默认3306）
unsigned int errno; // 最后一次操作的错误码
char *error;       // 最后一次操作的错误信息
// 其他内部状态（如连接状态、协议信息等）
} MYSQL;
```
**使用说明**：
必须通过 mysql_init() 初始化，不可直接声明使用。
连接成功后，所有后续操作（执行 SQL、获取结果等）都依赖此结构体。
### 2. MYSQL_RES 结构体
**作用**：存储查询结果集（如 SELECT 语句返回的结果），包含结果的行数、列数、字段信息等元数据。
主要成员（简化）：
```c
typedef struct st_mysql_res {
  my_ulonglong row_count; // 结果集中的行数
  unsigned int field_count; // 结果集中的列数
  MYSQL_FIELD *fields;    // 字段信息数组（描述每列的元数据）
  MYSQL_ROW *data;        // 存储结果行的数组（内部使用）
  // 其他内部状态（如当前行指针、内存管理信息等）
} MYSQL_RES;
```
**使用说明**：
由 mysql_store_result() 或 mysql_use_result() 函数返回。
使用完毕后必须通过 mysql_free_result() 释放，否则会导致内存泄漏。
### 3. MYSQL_ROW 结构体
**作用**：表示结果集中的一行数据，本质是一个字符串数组（每个元素对应一列的值）。
定义：
```c
typedef char **MYSQL_ROW; // 指向字符指针的指针（每行数据的列值数组）
```
**使用说明**：
通过 mysql_fetch_row() 从 MYSQL_RES 中获取，返回当前行的列值数组。
列值以字符串形式存储（即使是数字类型），需根据字段类型手动转换（如 atoi() 转整数）。
当无更多行时，返回 NULL。
### 4. MYSQL_FIELD 结构体
**作用**：描述结果集中一列的元数据（如列名、数据类型、长度等）。
主要成员（简化）：
```c
运行
typedef struct st_mysql_field {
  char *name;        // 列名
  char *table;       // 列所属的表名
  enum enum_field_types type; // 数据类型（如 MYSQL_TYPE_INT、MYSQL_TYPE_VARCHAR 等）
  unsigned int length; // 列的长度（定义时的长度）
  unsigned int decimals; // 小数位数（对数值类型有效）
  // 其他属性（如是否为 NULL、是否为主键等）
} MYSQL_FIELD;
```
**使用说明**：
通过 mysql_fetch_field() 或 mysql_fetch_fields() 从 MYSQL_RES 中获取。
用于动态处理结果集（如未知列名或类型时，通过字段信息解析数据）。
## 二、常用 C API 函数解析
按操作流程分为连接数据库、执行 SQL、处理结果、错误处理等类别。
### 1. 连接与初始化相关
#### mysql_init()
作用：初始化 MYSQL 结构体，为连接做准备。
原型：
```c
MYSQL *mysql_init(MYSQL *mysql);
```
参数：mysql 为 NULL 时，函数会自动分配新的 MYSQL 结构体。
返回值：成功返回 MYSQL*，失败返回 NULL。
#### mysql_real_connect()
作用：建立与 MySQL 服务器的实际连接。
原型：
```c
MYSQL *mysql_real_connect(
  MYSQL *mysql,         // mysql_init() 返回的句柄
  const char *host,     // 主机名（"localhost" 或 IP）
  const char *user,     // 用户名
  const char *passwd,   // 密码
  const char *db,       // 初始数据库名（NULL 表示不指定）
  unsigned int port,    // 端口号（0 表示默认 3306）
  const char *unix_socket, // Unix 套接字（NULL 表示不使用）
  unsigned long client_flag // 客户端标志（0 表示默认）
);
```
返回值：成功返回 mysql 指针，失败返回 NULL。
#### mysql_close()
作用：关闭数据库连接，释放 MYSQL 结构体占用的资源。
原型：
```c
void mysql_close(MYSQL *mysql);
```
### 2. 执行 SQL 相关
#### mysql_query()
作用：执行 SQL 语句（字符串需以 null 结尾）。
原型：
```c
int mysql_query(MYSQL *mysql, const char *stmt_str);
```
参数：stmt_str 为 SQL 语句（如 "SELECT * FROM users"）。
返回值：成功返回 0，失败返回非 0。
#### mysql_real_query()
作用：执行 SQL 语句（支持包含二进制数据的 SQL，需指定长度）。
原型：
```c
int mysql_real_query(
  MYSQL *mysql, 
  const char *stmt_str, 
  unsigned long length // SQL 语句的长度（字节数）
);
```
适用场景：SQL 中包含 null 字符（如二进制数据）时，mysql_query() 会提前终止，需用此函数。
### 3. 处理查询结果相关
#### mysql_field_count()
作用：返回最近一次查询所产生的结果集中的列数（字段数），基于当前 MYSQL 连接句柄的状态。
原型：
```c
unsigned int mysql_field_count(MYSQL *mysql);
```
参数：mysql 为初始化并连接后的 MYSQL 结构体指针。
返回值：
若最近一次查询返回结果集（如 SELECT），则返回该结果集的列数；
若最近一次查询不返回结果集（如 INSERT/UPDATE），则返回 0。
#### mysql_store_result()
作用：将查询结果（如 SELECT）全部读取到客户端内存，返回 MYSQL_RES 结果集。
原型：
```c
MYSQL_RES *mysql_store_result(MYSQL *mysql);
```
适用场景：结果集较小，需随机访问行（如获取行数）。
返回值：成功返回 MYSQL_RES*，失败返回 NULL。
#### mysql_use_result()
作用：初始化一个逐行读取的结果集（不将全部结果加载到内存）。
原型：
```c
MYSQL_RES *mysql_use_result(MYSQL *mysql);
```
适用场景：大结果集（避免内存占用过高），但需按顺序读取，且读取期间不能执行其他查询。
#### mysql_fetch_row()
作用：从 MYSQL_RES 中获取下一行数据，返回 MYSQL_ROW。
原型：
```c
MYSQL_ROW mysql_fetch_row(MYSQL_RES *result);
```
返回值：成功返回当前行的 MYSQL_ROW，无更多行时返回 NULL。
#### mysql_num_rows()
作用：获取结果集中的行数（仅对 mysql_store_result() 返回的结果集有效）。
原型：
```c
my_ulonglong mysql_num_rows(MYSQL_RES *result);
```
#### mysql_free_result()
作用：释放 MYSQL_RES 结果集占用的内存。
原型：
```c
void mysql_free_result(MYSQL_RES *result);
```
### 4. 错误处理相关
#### mysql_errno()
作用：返回最后一次操作的错误码。
原型：
```c
unsigned int mysql_errno(MYSQL *mysql);
```
#### mysql_error()
作用：返回最后一次操作的错误信息（字符串）。
原型：
```c
const char *mysql_error(MYSQL *mysql);
```
### 5. 其他常用函数
mysql_affected_rows()：获取受 INSERT/UPDATE/DELETE 影响的行数。
mysql_insert_id()：获取最后一次 INSERT 语句生成的自增 ID（需表中有自增列）。

