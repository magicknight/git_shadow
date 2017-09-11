#define _Z
#define _zDEBUG
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define _BSD_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/signal.h>
#include <pwd.h>

#include <pthread.h>
#include <sys/mman.h>

#include <sys/epoll.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <libgen.h>
#include <ctype.h>

#define zCommonBufSiz 1024
#include "../../inc/zutils.h"

#define zWatchHashSiz 8192  // 最多可监控的路径总数
#define zDpHashSiz 1009  // 布署状态HASH的大小，不要取 2 的倍数或指数，会导致 HASH 失效，应使用 奇数

#define zCacheSiz 1009
#define zPreLoadCacheSiz 10  // 版本批次及其下属的文件列表与内容缓存

/* 在zSendInfo之外，添加了：本地执行操作时需要，但对前端来说不必要的数据段 */
struct zRefDataInfo {
    struct zVecWrapInfo *p_SubVecWrapIf;  // 传递给 sendmsg 的下一级数据
    void *p_data;  // 当处于单个 Commit 记录级别时，用于存放 CommitSig 字符串格式，包括末尾的'\0'
};

/* 对 struct iovec 的封装，用于 zsendmsg 函数 */
struct zVecWrapInfo {
    _i VecSiz;
    struct iovec *p_VecIf;  // 此数组中的每个成员的 iov_base 字段均指向 p_RefDataIf 中对应的 p_SendIf 字段
    struct zRefDataInfo *p_RefDataIf;
};

struct zDpResInfo {
    _ui ClientAddr;  // 无符号整型格式的IPV4地址：0xffffffff
    _i RepoId;  // 所属代码库
    _i DpState;  // 布署状态：已返回确认信息的置为1，否则保持为0
    struct zDpResInfo *p_next;
};

/* 用于存放每个项目的元信息 */
struct zRepoInfo {
    _i RepoId;  // 项目代号
    char RepoPath[64];  // 项目路径，如："/home/git/miaopai_TEST"
    _i LogFd;  // 每个代码库的布署日志日志文件：log/sig，用于存储 SHA1-sig

    _i TotalHost;  // 每个项目的集群的主机数量

    pthread_rwlock_t RwLock;  // 每个代码库对应一把全局读写锁，用于写日志时排斥所有其它的写操作
    pthread_rwlockattr_t zRWLockAttr;  // 全局锁属性：写者优先

    void *p_MemPool;  // 线程内存池，预分配 16M 空间，后续以 8M 为步进增长
    size_t MemPoolSiz;  // 内存池初始大小：8M
    pthread_mutex_t MemLock;  // 内存池锁
    _ui MemPoolHeadId;  // 动态指示下一次内存分配的起始地址

    _i CacheId;

    /* 0：非锁定状态，允许布署或撤销、更新ip数据库等写操作 */
    /* 1：锁定状态，拒绝执行布署、撤销、更新ip数据库等写操作，仅提供查询功能 */
    _i DpLock;

    _i ReplyCnt;  // 用于动态汇总单次布署或撤销动作的统计结果
    pthread_mutex_t MutexLock;  // 用于保证 ReplyCnt 计数的正确性

    struct zDpResInfo *p_DpResList;  // 布署状态收集
    struct zDpResInfo *p_DpResHash[zDpHashSiz];  // 对上一个字段每个值做的散列

    _i CommitCacheQueueHeadId;  // 用于标识提交记录列表的队列头索引序号（index）
    struct zVecWrapInfo CommitVecWrapIf;  // 存放 commit 记录的原始队列信息
    struct iovec CommitVecIf[zCacheSiz];
    struct zRefDataInfo CommitRefDataIf[zCacheSiz];

    struct zVecWrapInfo SortedCommitVecWrapIf;  // 存放经过排序的 commit 记录的缓存队列信息
    struct iovec SortedCommitVecIf[zCacheSiz];

    struct zVecWrapInfo DpVecWrapIf;  // 存放 deploy 记录的原始队列信息
    struct iovec DpVecIf[zCacheSiz];
    struct zRefDataInfo DpRefDataIf[zCacheSiz];
};

struct zRepoInfo *zpGlobRepoIf;

/************
 * 全局变量 *
 ************/
_i zGlobRepoNum;  // 总共有多少个代码库

#define UDP 0
#define TCP 1

/************
 * 配置文件 *
 ************/
_i
ztry_connect(struct sockaddr *zpAddr, socklen_t zLen, _i zSockType, _i zProto) {
    if (zSockType == 0) { zSockType = SOCK_STREAM; }
    if (zProto == 0) { zProto = IPPROTO_TCP; }

    _i zSd = socket(AF_INET, zSockType, zProto);
    zCheck_Negative_Return(zSd, -1);
    for (_i i = 4; i > 0; --i) {
        if (0 == connect(zSd, zpAddr, zLen)) { return zSd; }
        shutdown(zSd, SHUT_RDWR);
        sleep(i);
    }

    return -1;
}

struct addrinfo *
zgenerate_hint(_i zFlags) {
    static struct addrinfo zHints;
    zHints.ai_flags = zFlags;
    zHints.ai_family = AF_INET;
    return &zHints;
}

_i
ztcp_connect(char *zpHost, char *zpPort, _i zFlags) {
    struct addrinfo *zpRes, *zpTmp, *zpHints;
    _i zSockD, zErr;

    zpHints = zgenerate_hint(zFlags);

    zErr = getaddrinfo(zpHost, zpPort, zpHints, &zpRes);
    if (-1 == zErr){ zPrint_Err(errno, NULL, gai_strerror(zErr)); }

    for (zpTmp = zpRes; NULL != zpTmp; zpTmp = zpTmp->ai_next) {
        if(0 < (zSockD  = ztry_connect(zpTmp->ai_addr, INET_ADDRSTRLEN, 0, 0))) {
            freeaddrinfo(zpRes);
            return zSockD;
        }
    }

    freeaddrinfo(zpRes);
    return -1;
}

_i
zsendto(_i zSd, void *zpBuf, size_t zLen, _i zFlags, struct sockaddr *zpAddr) {
    _i zSentSiz = sendto(zSd, zpBuf, zLen, 0 | zFlags, zpAddr, INET_ADDRSTRLEN);
    zCheck_Negative_Return(zSentSiz, -1);
    return zSentSiz;
}

_i
zrecv_all(_i zSd, void *zpBuf, size_t zLen, _i zFlags, struct sockaddr *zpAddr) {
    socklen_t zAddrLen;
    _i zRecvSiz = recvfrom(zSd, zpBuf, zLen, MSG_WAITALL | zFlags, zpAddr, &zAddrLen);
    zCheck_Negative_Return(zRecvSiz, -1);
    return zRecvSiz;
}

#define zBufSiz 10240
void
zclient(void) {
    _i zSd = ztcp_connect("192.168.1.254", "20000", AI_NUMERICHOST | AI_NUMERICSERV);
    if (-1 == zSd) {
        fprintf(stderr, "Connect to server failed \n");
        exit(1);
    }

    // 列出所有项目元信息
    //char zStrBuf[] = "{\"OpsId\":5}";

    // 列出单个项目元信息
    //char zStrBuf[] = "{\"OpsId\":6,\"ProjId\":11}";

    // 创建新项目
    //char zStrBuf[] = "{\"OpsId\":1,\"data\":\"11 /home/git/11_Y https://git.coding.net/kt10/FreeBSD.git master git\"}";

    // 锁定
    //char zStrBuf[] = "{\"OpsId\":2,\"ProjId\":11}";

    // 解锁
    //char zStrBuf[] = "{\"OpsId\":3,\"ProjId\":11}";

    // 重置（删除中转机与目标机上的项目文件，清空内存中的中转IP与目标IP列表）
    //char zStrBuf[] = "{\"OpsId\":14,\"ProjId\":11,\"data\":\"_\",\"ExtraData\":0}";
    
    // 更新 proxy IP 数据
    //char zStrBuf[] = "{\"OpsId\":4,\"ProjId\":11,\"data\":\"192.168.1.254\"}";

    // 查询版本号列表
    //char zStrBuf[] = "{\"OpsId\":9,\"ProjId\":11,\"DataType\":0}";

    // 打印差异文件列表
    //char zStrBuf[] = "{\"OpsId\":10,\"ProjId\":11,\"RevId\":1,\"CacheId\":1000000000,\"DataType\":0}";

    // 打印差异文件内容
    //char zStrBuf[] = "{\"OpsId\":11,\"ProjId\":11,\"RevId\":0,\"FileId\":0,\"CacheId\":1000000000,\"DataType\":0}";

    // 布署与撤销
    //char zStrBuf[] = "{\"OpsId\":12,\"ProjId\":11,\"RevId\":1,\"CacheId\":1000000000,\"DataType\":0,\"data\":\"172.16.0.1|172.16.0.2|172.16.0.3|172.16.0.4|172.16.0.5|172.16.0.6|172.16.0.7|172.16.0.8|172.16.0.9|172.16.0.10|172.16.0.11|172.16.0.12|172.16.0.13|172.16.0.14|172.16.0.15|172.16.0.16|172.16.0.17|172.16.0.18|172.16.0.19|172.16.0.20|172.16.0.21|172.16.0.22|172.16.0.23|172.16.0.24|172.16.0.25|172.16.0.26|172.16.0.27|172.16.0.28|172.16.0.29|172.16.0.30|172.16.0.31|172.16.0.32|172.16.0.33|172.16.0.34|172.16.0.35|172.16.0.36|172.16.0.37|172.16.0.38|172.16.0.39|172.16.0.40|172.16.0.41\",\"ExtraData\":41}";

    // 新加入的主机请求布署自身
    //char zStrBuf[] = "{\"OpsId\":13,\"ProjId\":11,\"data\":172.16.0.1,\"ExtraData\":1}";

    zsendto(zSd, zStrBuf, strlen(zStrBuf), 0, NULL);

    char zBuf[zBufSiz] = {'\0'};

    while (0 < recv(zSd, &zBuf, zBufSiz, 0)) {
        for (_i i = 0; i < zBufSiz; i++) {
            fprintf(stderr, "%c", zBuf[i]);
        }
        memset(zBuf, 0, zBufSiz);
    }


    shutdown(zSd, SHUT_RDWR);
}

_i
main(_i zArgc, char **zppArgv) {
    zArgc = 0;
    zppArgv = NULL;
    zclient();
    return 0;
}
