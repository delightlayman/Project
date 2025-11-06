#ifndef RAFT_H
#define RAFT_H

#include <atomic>
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
constexpr int Voted = 1;  // 本轮已投过票
constexpr int Expire = 2; // 投票（消息、竞选者）过期
constexpr int Normal = 3;

// 在 Raft 集群中，每个raft节点同时扮演 RPC 客户端和服务端的双重角色，这是由分布式系统中节点 “对等交互” 的特性决定的
// Raft 节点之间需要双向通信：每个节点既需要主动向其他节点发送 RPC 请求（客户端角色），
// 也需要接收并处理其他节点发来的 RPC 请求（服务端角色），二者缺一不可
class Raft : public raftRpcProctoc::raftRpc
{
private:
  std::mutex m_mtx; // 互斥锁

  // m_peers:当前Raft节点与集群中所有其他节点通信的 “RPC连接池”，存储了与每个同伴节点的RPC交互工具，每个节点都必须拥有它
  // 因为 Raft 是分布式协议，节点需要和集群中所有同伴主动交互或响应，才能完成选举、日志同步等核心流程。
  std::vector<std::shared_ptr<RaftRpcUtil>> m_peers; // 用于通信的rpc连接池
  std::vector<raftRpcProctoc::LogEntry> m_logs;      // 日志条目数组，包含了状态机要执行的指令集，以及收到领导时的任期号
  std::shared_ptr<Persister> m_persister;            // 持久层，用于持久化数据

  int m_me;          // 编号---raft以集群启动---自身标记
  int m_currentTerm; // 当前任期
  int m_votedFor;    // 当前任期投票给谁

  // 所有结点维护---从0开始
  int m_commitIndex; // 提交的log的index
  int m_lastApplied; // 已经汇报给状态机（上层应用）的log 的index

  // leader维护---从1开始
  std::vector<int> m_nextIndex;  // 下一个要发送给follower的节点索引
  std::vector<int> m_matchIndex; // 追随者返回给leader的日志条目索引

  enum Status
  {
    Follower,
    Candidate,
    Leader
  };
  // 当前身份/状态
  Status m_status;
  std::shared_ptr<LockQueue<ApplyMsg>> applyChan; // client从这里取日志（2B），client与raft通信的接口

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
  // 定期向状态机写入日志
  void applierTicker();
  // 记录某个时刻的状态---快照
  bool CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot);

  // ****** 领导选举 *******
  // 选举超时定时器（此函数）
  // 每隔一段时间检查睡眠时间内有没有重置选举超时定时器
  // 没有则说明选举超时,发起选举
  // 有则设置合适睡眠时间：睡眠到重置时间+超时时间
  void electionTimeOutTicker(); // 选举超时检测与触发选举
  //  发起选举---构建需要发送的rpc，然后使用多线程调用sendRequestVote处理rpc和响应
  void doElection();
  // candidate发送投票请求---其他节点为自己投票
  // 发送请求投票RPC，发送rpc后再接收并处理对端发送回来的响应
  bool sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                       std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<std::atomic<int>> votedNum);
  // follower处理candidate的投票请求，是否要给candidate投票。
  void RequestVote(const raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *reply);

  // ****** 心跳机制 *******
  // 监控是否需要发送心跳，是则doHeartBeat
  void leaderHearBeatTicker();
  // 发起心跳，只有leader才需要发起心跳
  void doHeartBeat();

  // ****** 日志同步 *******
  // 发送追加日志条目RPC，在发送完RPC后还需要负责接收并处理对端发送回来的响应。
  bool sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                         std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply, std::shared_ptr<int> appendNums);
  // 接收leader发送的日志请求，主要检验用于检查当前日志是否匹配并同步leader的日志到本机。
  void AppendEntries1(const raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *reply);

  // ****** 快照 *******
  // 发送快照的RPC，在发送完RPC后还需要负责接收并处理对端发送回来的响应。
  void leaderSendSnapShot(int server);
  // 接收leader发送的快照请求，同步快照到本机。
  void InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args,
                       raftRpcProctoc::InstallSnapshotResponse *reply);

  void persist();            // 持久化raft节点的状态
  std::string persistData(); // 持久化faft节点状态的实际数据---persist()内部调用

  std::vector<ApplyMsg> getApplyLogs();                         // 获取应用日志
  int getNewCommandIndex();                                     // 获取新命令的index
  void getPrevLogInfo(int server, int *preIndex, int *preTerm); // 获取当前日志信息
  void GetState(int *term, bool *isLeader);                     // 获取当前状态---是否是leader

  void leaderUpdateCommitIndex();           // leader更新commitIndex
  bool matchLog(int logIndex, int logTerm); // 对象日志是否匹配---判断leader日志和follower日志是否匹配

  bool UpToDate(int index, int term);                               // 判断当前节点是否为最新日志
  int getLastLogIndex();                                            // 获取最后一个日志的index
  int getLastLogTerm();                                             // 获取最后一个日志的term
  void getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm); // 获取最后一个日志的index和term
  int getLogTermFromLogIndex(int logIndex);                         // 获取指定日志index的term
  int GetRaftStateSize();                                           // 获取当前状态大小
  int getSlicesIndexFromLogIndex(int logIndex);                     // 从日志index获取日志条目在m_logs中位置

  // rf.applyChan <- msg
  // 不加锁执行，可以单独创建一个线程执行，统一使用std:thread，而非pthread_create
  void pushMsgToKvServer(ApplyMsg msg); // 发送消息到上层kvserver
  void readPersist(std::string data);   // 读取持久化的数据

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
  // rpc服务端实现
  // 重写rpc方法---rpc方法的具体实现，rpc客服端远端调用的就是这些方法
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
  // ****** 持久化 ******
  class BoostPersistRaftNode
  {
  public:
    friend class boost::serialization::access;

    // Boost.Serialization 为归档对象（Archive 类型，如输出归档 boost::archive::text_oarchive
    // 或输入归档 boost::archive::text_iarchive）重载了 & 运算符，
    // 使其同时支持 “写入”（序列化）和“读取”（反序列化）操作：
    // 当 Archive 是输出归档（如用于序列化的 oarchive）时，ar & x 等价于 ar << x，表示将 x 的数据写入归档中。
    // 当 Archive 是输入归档（如用于反序列化的 iarchive）时，ar & x 等价于 ar >> x，表示从归档中读取数据并赋值给 x。
    template <class Archive>
    void serialize(Archive &ar, const unsigned int version)
    {
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
    std::vector<std::string> m_logs;
  };
};

#endif // RAFT_H