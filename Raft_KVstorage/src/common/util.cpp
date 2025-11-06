#include "util.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>

void myAssert(bool condition, std::string message) {
  if (!condition) {
    std::cerr << "Error: " << message << std::endl;
    std::exit(EXIT_FAILURE);//退出进程
  }
}

std::chrono::_V2::system_clock::time_point now() { return std::chrono::high_resolution_clock::now(); }

// 生成一个随机的毫秒级选举超时时间---用于分布式协议（如 Raft）中避免多个节点同时触发选举（防止 “分裂投票”）
std::chrono::milliseconds getRandomizedElectionTimeout() {
  // 获取随机种子
  // 获取 “真随机数种子” 的设备（通常依赖系统底层的随机源，如硬件噪声、用户输入等）。
  // 注意：在某些没有真随机源的平台上，它可能退化为伪随机数，但仍能提供初始种子。
  std::random_device rd;
  // 初始化随机数生成器
  //std::mt19937是基于 “梅森旋转算法” 的伪随机数生成器（PRNG）
  //用rd()生成的随机种子初始化rng，确保每次程序运行（或函数调用）时生成的随机序列尽可能不同。
  std::mt19937 rng(rd());
  // 定义随机分布的范围：std::uniform_int_distribution<int> dist(min, max)
  // std::uniform_int_distribution<int>是一个 “均匀整数分布” 模板类，用于生成在[min, max]闭区间内的整数，且每个整数的概率相等（均匀分布）
  std::uniform_int_distribution<int> dist(minRandomizedElectionTime, maxRandomizedElectionTime);

  // dist(rng)通过分布对象dist从随机数生成器rng中获取一个随机整数（范围[min, max]），
  // 然后用这个整数构造std::chrono::milliseconds对象并返回 —— 即最终的随机选举超时时间（毫秒级）。
  return std::chrono::milliseconds(dist(rng));
}

void sleepNMilliseconds(int N) { std::this_thread::sleep_for(std::chrono::milliseconds(N)); };

bool getReleasePort(short &port) {
  short num = 0;
  while (!isReleasePort(port) && num < 30) {
    ++port;
    ++num;
  }
  if (num >= 30) {
    port = -1;
    return false;
  }
  return true;
}

bool isReleasePort(unsigned short usPort) {
  int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(usPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int ret = ::bind(s, (sockaddr *)&addr, sizeof(addr));
  if (ret != 0) {
    close(s);
    return false;
  }
  close(s);
  return true;
}

void DPrintf(const char *format, ...) {
  if (Debug) {
    // 获取当前的日期，然后取日志信息，写入相应的日志文件当中 a+
    time_t now = time(nullptr);
    tm *nowtm = localtime(&now);
    va_list args;
    va_start(args, format);
    std::printf("[%d-%d-%d-%d-%d-%d] ", nowtm->tm_year + 1900, nowtm->tm_mon + 1, nowtm->tm_mday, nowtm->tm_hour,
                nowtm->tm_min, nowtm->tm_sec);
    std::vprintf(format, args);
    std::printf("\n");
    va_end(args);
  }
}
