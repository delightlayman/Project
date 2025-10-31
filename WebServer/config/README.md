### 一、getopt()的核心功能
getopt()的作用是从命令行参数列表（argc和argv）中解析出选项（option） 和选项参数（option argument），区分 “选项参数”（如-a、-b value）和 “非选项参数”（如文件名、路径等普通参数）。

例如，对于命令./program -v -o output.txt input.dat，getopt()会识别出-v、-o（及其参数output.txt）为选项，input.dat为非选项参数。
### 二、函数原型与参数
```cpp
#include <unistd.h>
int getopt(int argc, char *const argv[], const char *optstring);
```
- argc：命令行参数总数（与main函数的argc一致）。
- argv：命令行参数数组（与main函数的argv一致）。
- optstring：字符串，定义合法选项及格式，格式规则：
  - 单个字符表示一个选项（如"ab"表示支持-a、-b）。
  - 字符后接:表示该选项必须带参数（如"b:"表示-b需要参数，如-b 100）。
  - 字符后接::表示该选项参数可选（GNU 扩展，如"c::"表示-c可带参数或不带）。
### 三、关键全局变量
getopt()通过以下全局变量返回解析过程中的附加信息：
变量名	含义
- optind	下一个待解析的参数在argv中的索引（初始值为 1，解析完成后指向非选项参数）。
- optarg	当选项需要参数时，指向该参数的指针（如-b 100中，optarg为"100"）。
- opterr	若为非 0（默认），getopt()会自动向stderr输出错误信息；设为 0 则不输出。
- optopt	当解析到无效选项时，存储该无效选项字符（供错误处理使用）。
### 四、返回值规则
getopt()每次调用返回一个当前解析到的选项字符，解析结束时返回-1。具体规则：
- 成功解析到合法选项：返回该选项字符（如'a'、'b'）。
- 解析到无效选项（不在optstring中）：返回'?'，optopt存储该无效选项。
- 解析到需要参数但未提供参数的选项（如-b后无参数）：返回'?'（若optstring以:开头，则返回':'，便于区分错误类型）。
- 所有选项解析完成：返回-1，剩余参数为非选项参数（可通过argv[optind...]访问）。