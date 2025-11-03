#ifndef RAFT_H
#define RAFT_H

#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ApplyMsg.h"
#include "Persister.h"
#include "boost/any.hpp"
#include "boost/serialization/serialization.hpp"
#include "config.h"
#include "monsoon.h"
#include "raftRpcUtil.h"
#include "util.h"

// 网络异常：disconnected，网络正常：AppNormal，防止matchIndex[]数组异常减小
constexpr int Disconnected = 0;
constexpr int AppNormal = 1;

// 投票状态
constexpr int Killed = 0;
constexpr int Voted = 1;   // 本轮已投过票
constexpr int Expire = 2;  // 投票（消息、竞选者）过期
constexpr int Normal = 3;

class Raft : public raftRpcProctoc::raftRpc {
 private:
  std::mutex m_mtx;
  std::vector<std::shared_ptr<RaftRpcUtil>> m_peers;  // 用于通信的rpc对象
  std::shared_ptr<Persister> m_persister;             // 持久层，用于持久化数据
  int m_me;                                           // 编号---raft以集群启动---自身标记
  int m_currentTerm;                                  // 当前任期
  int m_votedFor;                                     // 当前任期投票给谁
  std::vector<raftRpcProctoc::LogEntry> m_logs;  // 日志条目数组，包含了状态机要执行的指令集，以及收到领导时的任期号

  // 所有结点维护---从0开始
  int m_commitIndex;  // 提交的log的index
  int m_lastApplied;  // 已经汇报给状态机（上层应用）的log 的index

  // leader维护---从1开始
  std::vector<int> m_nextIndex;   // 下一个要发送给follower的节点索引
  std::vector<int> m_matchIndex;  // 追随者返回给leader的日志条目索引
  enum Status { Follower, Candidate, Leader };
  // 当前身份/状态
  Status m_status;
  std::shared_ptr<LockQueue<ApplyMsg>> applyChan;  // client从这里取日志（2B），client与raft通信的接口

  // 上一次 “重置选举超时” 的时间点
  // 重置条件：follower收到leader的AppendEntry RPC（日志同步或心跳）
  // 作用：判定是否需要重置选举超时定时器---electionTimeOutTicker()内部是否continue所在while循环
  std::chrono::_V2::system_clock::time_point m_lastResetElectionTime;
  // 上一次 “重置心跳超时” 的时间点---用于leader
  std::chrono::_V2::system_clock::time_point m_lastResetHearBeatTime;

  // 储存了快照中的最后一个日志的Index和Term
  int m_lastSnapshotIncludeIndex;
  int m_lastSnapshotIncludeTerm;

  // 协程
  std::unique_ptr<monsoon::IOManager> m_ioManager = nullptr;

 public:
  // 日志同步 + 心跳
  void AppendEntries1(const raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *reply);
  // 定期向状态机写入日志
  void applierTicker();
  // 记录某个时刻的状态---快照
  bool CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot);

  // 发起选举---构建需要发送的rpc，然后使用多线程调用sendRequestVote处理rpc和响应
  void doElection();
  // 发起心跳，只有leader才需要发起心跳
  void doHeartBeat();

  // 选举超时定时器（此函数）
  // 每隔一段时间检查睡眠时间内有没有重置选举超时定时器
  // 没有则说明选举超时,发起选举
  // 有则设置合适睡眠时间：睡眠到重置时间+超时时间
  void electionTimeOutTicker();  // 选举超时检测与触发选举

  std::vector<ApplyMsg> getApplyLogs();                                     // 获取应用日志
  int getNewCommandIndex();                                                 // 获取新命令的index
  void getPrevLogInfo(int server, int *preIndex, int *preTerm);             // 获取当前日志信息
  void GetState(int *term, bool *isLeader);                                 // 获取当前状态---是否是leader
  void InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args,  // 安装快照
                       raftRpcProctoc::InstallSnapshotResponse *reply);
  void leaderHearBeatTicker();               // 监控是否需要发送心跳，是则doHeartBeat
  void leaderSendSnapShot(int server);       // leader发送快照
  void leaderUpdateCommitIndex();            // leader更新commitIndex
  bool matchLog(int logIndex, int logTerm);  // 对象日志是否匹配---判断leader日志和follower日志是否匹配
  void persist();                            // 持久化当前状态

  // 接收别人发来的选举请求，检验是否要给对方投票。
  void RequestVote(const raftRpcProctoc::RequestVoteArgs *args,  // 请求投票
                   raftRpcProctoc::RequestVoteReply *reply);
  bool UpToDate(int index, int term);                                // 判断当前节点是否为最新日志
  int getLastLogIndex();                                             // 获取最后一个日志的index
  int getLastLogTerm();                                              // 获取最后一个日志的term
  void getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm);  // 获取最后一个日志的index和term
  int getLogTermFromLogIndex(int logIndex);                          // 获取指定日志index的term
  int GetRaftStateSize();                                            // 获取当前状态大小
  int getSlicesIndexFromLogIndex(int logIndex);                      // 从日志index获取日志条目在m_logs中位置

  // 发送投票请求---其他节点为自己投票
  // 发送选举中的RPC，发送rpc后再接收并处理对端发送回来的响应
  bool sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                       std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum);
  // 发送追加日志条目
  bool sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                         std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply, std::shared_ptr<int> appendNums);

  // rf.applyChan <- msg
  // 不加锁执行，可以单独创建一个线程执行，统一使用std:thread，而非pthread_create
  void pushMsgToKvServer(ApplyMsg msg);  // 发送消息到上层kvserver
  void readPersist(std::string data);    // 读取持久化的数据
  std::string persistData();             // 持久化数据

  void Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader);

  // Snapshot the service says it has created a snapshot that has
  // all info up to and including index. this means the
  // service no longer needs the log through (and including)
  // that index. Raft should now trim its log as much as possible.
  // index：快照apply应用的index,snapshot：上层service传来的快照字节流，包括了Index之前的数据
  // 即服务层主动发起请求raft保存snapshot里面的数据，index是用来表示snapshot快照执行到了哪条命令
  // 作用：把安装到快照里的日志抛弃，并安装快照数据，同时更新快照下标---属于peers自身主动更新，与leader发送快照不冲突
  void Snapshot(int index, std::string snapshot);

 public:
  // 重写基类方法---rpc远程调用真正调用的是这个方法
  // 序列化，反序列化等操作rpc框架都已经做完了，因此这里只需要获取值然后真正调用本地方法即可。
  void AppendEntries(google::protobuf::RpcController *controller, const ::raftRpcProctoc::AppendEntriesArgs *request,
                     ::raftRpcProctoc::AppendEntriesReply *response, ::google::protobuf::Closure *done) override;
  void InstallSnapshot(google::protobuf::RpcController *controller,
                       const ::raftRpcProctoc::InstallSnapshotRequest *request,
                       ::raftRpcProctoc::InstallSnapshotResponse *response, ::google::protobuf::Closure *done) override;
  void RequestVote(google::protobuf::RpcController *controller, const ::raftRpcProctoc::RequestVoteArgs *request,
                   ::raftRpcProctoc::RequestVoteReply *response, ::google::protobuf::Closure *done) override;

 public:
  void init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
            std::shared_ptr<LockQueue<ApplyMsg>> applyCh);

 private:
  // for persist

  class BoostPersistRaftNode {
   public:
    friend class boost::serialization::access;
    // When the class Archive corresponds to an output archive, the
    // & operator is defined similar to <<.  Likewise, when the class Archive
    // is a type of input archive the & operator is defined similar to >>.

    // Boost.Serialization 为归档对象（Archive 类型，如输出归档 boost::archive::text_oarchive
    // 或输入归档 boost::archive::text_iarchive）重载了 & 运算符，
    // 使其同时支持 “写入”（序列化）和“读取”（反序列化）操作：
    // 当 Archive 是输出归档（如用于序列化的 oarchive）时，ar & x 等价于 ar << x，表示将 x 的数据写入归档中。
    // 当 Archive 是输入归档（如用于反序列化的 iarchive）时，ar & x 等价于 ar >> x，表示从归档中读取数据并赋值给 x。
    template <class Archive>
    void serialize(Archive &ar, const unsigned int version) {
      ar & m_currentTerm;
      ar & m_votedFor;
      ar & m_lastSnapshotIncludeIndex;
      ar & m_lastSnapshotIncludeTerm;
      ar & m_logs;
    }
    int m_currentTerm;
    int m_votedFor;
    int m_lastSnapshotIncludeIndex;
    int m_lastSnapshotIncludeTerm;
    // 注意：这里存储的是日志条目的字节流string---理论上应该与raft的m_logs保持一致:LogEntry
    // raftRpcProctoc::LogEntry 通常是 Protobuf 生成的类（包含 term、command 等字段），
    // 而 std::vector<std::string> 无法直接存储其完整信息，会丢失数据。
    std::vector<std::string> m_logs;
    std::unordered_map<std::string, int> umap;  // 是否用上？若用上是否需要序列化？
  };
};

#endif  // RAFT_H