#ifndef RAFTRPC_H
#define RAFTRPC_H

#include "raftRPC.pb.h"

// 维护当前节点对其他某一个结点的所有rpc发送通信的功能
// 对于一个raft节点来说，对于任意其他的节点都要维护一个rpc连接，即MprpcChannel
// rpc的客服端实现
class RaftRpcUtil {
 private:
  raftRpcProctoc::raftRpc_Stub *stub_;

 public:
  //主动调用其他节点的三个方法,可以按照mit6824来调用。节点调用自己，或许要继承protoc提供的service类才行
  bool AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response);
  bool InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapshotResponse *response);
  bool RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response);
  
  //远端ip，远端端口
  RaftRpcUtil(std::string ip, short port);
  ~RaftRpcUtil();
};

#endif  // RAFTRPC_H
