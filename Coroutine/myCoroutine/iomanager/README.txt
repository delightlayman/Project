fcntl
---修改或查询与文件描述符关联的动态操作状态
---无法修改文件的静态元数据（如权限、所有者、文件大小等，这些由 inode 管理），
---也无法修改文件的访问模式（O_RDONLY/O_WRONLY/O_RDWR，这些在 open() 时确定，若需变更需重新打开文件）

文件状态标志（F_GETFL / F_SETFL）：O_APPEND,O_NONBLOCK,O_ASYNC,O_DIRECT,O_NOATIME
1.覆盖原有标志（如O_APPEND，O_ASYNC 等标志，不特别处理的话会被清除）。
    fcntl(_tickleFds[0], F_SETFL, O_NONBLOCK) 
2.在保留原有状态标志的基础上添加 O_NONBLOCK（而不是直接覆盖）
    int flags = fcntl(_tickleFds[0], F_GETFL);  // 获取当前状态标志
    if (flags == -1) { /* 错误处理 */ }
    flags |= O_NONBLOCK;  // 保留原有标志，添加非阻塞模式
    int rt = fcntl(_tickleFds[0], F_SETFL, flags);  // 重新设置

