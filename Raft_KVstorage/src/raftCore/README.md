### raft选举流程
领导选举：sendRequestVote RequestVote
日志同步：sendAppendEntries AppendEntries
### 定时器维护
raft定时向状态机写入：applierTicker
心跳定时器维护：leaderHeartbeatTicker
选举超时定时器：electionTimeoutTicker
### 持久化
持久化内容：
持久化时机：persist