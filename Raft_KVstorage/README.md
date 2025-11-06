
### 项目结构
```bash
.
├── bin 生成的可执行文件存放地
├── cmake-build-debug 项目编译目录，默认是没有的，需要自己创建
├── docs    项目文档存放地
│   └── images  项目文档图片存放地
├── example  范例代码存放地
│   ├── fiberExample  协程相关代码
│   ├── raftCoreExample raft核心代码
│   └── rpcExample rpc相关代码
├── lib  项目编译后的库文件存放地
├── src 【重点】项目源代码存放地，按照子模块组织
│   ├── common  子模块共用的，一般是一些util，日志，配置文件
│   ├── fiber  协程相关代码
│   ├── raftClerk raft客户端代码
│   ├── raftCore raft核心代码
│   ├── raftRpcPro raft中rpc涉及的protoc文件
│   ├── rpc  rpc库相关代码
│   └── skipList 跳表（上层状态机）相关代码
└── test  测试代码存放地，作用不大，一般是对一些不确定的特性进行测试
```
### 运行
```bash
cd Ratf_KVstorage/   # 进入项目根目录
mkdir cmake-build-debug # 创建编译目录
cd cmake-build-debug
cmake ..
make
```
1. RPC测试
   
```bash
# 终端1
cd bin
./provider
# 终端2
cd bin
./consumer
```
2. raft测试

```bash
cd bin
./raftCoreRun -n 3 -f test.conf
```

3. 跳表KV存储测试

```bash
cd bin
# 启动集群
./raftCoreRun -n 3 -f test.conf
# 测试跳表KV
./callermain
```





