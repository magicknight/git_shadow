#ifndef ZDPOPS_H
#define ZDPOPS_H

#ifndef _Z_BSD
    #ifndef _XOPEN_SOURCE
    #define _XOPEN_SOURCE 700
    #endif

    #ifndef _DEFAULT_SOURCE
    #define _DEFAULT_SOURCE
    #endif

    #ifndef _BSD_SOURCE
    #define _BSD_SOURCE
    #endif
#endif

#include <pthread.h>  //  该头文件内部已使用 #define _PTHREAD_H 避免重复
#include <semaphore.h>  //  该头文件内部已使用 #define _SEMAPHORE_H 避免重复
#include <libpq-fe.h>  //  该头文件内部已使用 #define LIBPQ_FE_H 避免重复

#include "zCommon.h"
#include "zNativeUtils.h"
#include "zNetUtils.h"

#include "zLibSsh.h"
#include "zLibGit.h"

#include "zNativeOps.h"

#include "zPosixReg.h"
#include "zPgSQL.h"
#include "zThreadPool.h"
//#include "zMd5Sum.h"
#include "cJSON.h"

#define zGlobRepoNumLimit 256  // 可以管理的代码库数量上限
#define zGlobRepoIdLimit 10 * 256  // 代码库 ID 上限
#define zCacheSiz 64  // 顶层缓存单元数量取值不能超过 IOV_MAX
#define zDpTraficLimit 256  // 同一项目可同时发出的 push 连接数量上限
#define zDpHashSiz 1009  // 布署状态HASH的大小，不要取 2 的倍数或指数，会导致 HASH 失效，应使用 奇数
#define zSendUnitSiz 8  // sendmsg 单次发送的单元数量，在 Linux 平台上设定为 <=8 的值有助于提升性能
#define zForecastedHostNum 200  // 预测的目标主机数量上限

#define zGlobCommonBufSiz 1024

#define zDpUnLock 0
#define zDpLocked 1

#define zCacheGood 0
#define zCacheDamaged 1

#define zIsCommitDataType 0
#define zIsDpDataType 1

typedef struct __zThreadPool__ {
    pthread_t selfTid;
    pthread_cond_t condVar;

    void * (* func) (void *);
    void *p_param;
} zThreadPool__;

typedef struct __zDpRes__ {
    /*
     * unsigned long long int
     * IPv4 地址只使用第一个成员
     */
    _ull clientAddr[2];

    /*
     * << 布署状态 >>
     * bit[0]:目标端初始化(SSH)成功
     * bit[1]:服务端本地布署动作(git push)成功
     * bit[2]:目标端已收到推送内容(post-update)
     * bit[3]:目标端已确认内容无误(post-update)
     * bit[4]:目标端已确认布署后动作执行成功
     * bit[5]:
     * bit[6]:
     * bit[7]:
     */
    _uc resState;

    /*
     * << 错误类型 >>
     * err1 bit[0]:服务端错误
     * err2 bit[1]:网络不通
     * err3 bit[2]:SSH 连接认证失败
     * err4 bit[3]:目标端磁盘容量不足
     * err5 bit[4]:目标端权限不足
     * err6 bit[5]:目标端文件冲突
     * err7 bit[6]:目标端布署后动作执行失败
     * err8 bit[7]:目标端收到重复布署指令(同一目标机的多个不同IP)
     * err9 bit[8]:目标机 IP 格式错误/无法解析
     */
    _ui errState;

    /*
     * 存放本地产生的或目标机返回的错误信息
     */
    char errMsg[256];

    struct __zDpRes__ *p_next;
} zDpRes__;

typedef struct __zDpCcur__ {
    /*
     * 必须放置在首位
     * 线程池会将此指针指向每个线程的元信息
     * 清理特定线程时会用到
     */
    zThreadPool__ *p_threadSource_;

    _i repoId;

    /*
     * 单次动作的身份唯一性标识
     * 布署时为：time_stamp
     */
    _l id;

    /* 单个目标机 Ip，如："10.0.0.1" "::1" */
    char *p_hostIpStrAddr;

    /* 字符串形式的端口号，如："22" */
    char *p_hostServPort;

    /* 需要执行的 SSH 指令集合 */
    char *p_cmd;

    /* SSH 认证类型：公钥或密码 */
    zAuthType__ authType;

    /* 目标机上的用户名称 */
    const char *p_userName;

    /* 服务器上公钥所在路径，如："/home/git/.ssh/id_rsa.pub" */
    const char *p_pubKeyPath;

    /* 服务器上私钥所在路径，如："/home/git/.ssh/id_rsa" */
    const char *p_privateKeyPath;

    /*
     * 公钥认证时，用于提定公钥的密码
     * 密码认证时，用于指定用户登陆密码
     * 留空表示无密码
     */
    const char *p_passWd;

    /* 用于存放远程 SSH 命令的回显信息 */
    char *p_remoteOutPutBuf;
    _ui remoteOutPutBufSiz;

    /* libssh2 中的部分环节需要加锁，才能并发 */
    pthread_mutex_t *p_ccurLock;

    /* 指向目标机 IP 在服务端 HASH 链中的节点 */
    zDpRes__ *p_selfNode;
} zDpCcur__;


/* 用于存放本地操作时需要，但前端不需要的数据 */
typedef struct __zRefData__ {
    /* 传递给 sendmsg 的下一级数据 */
    struct __zVecWrap__ *p_subVecWrap_;

    /* 指向实际的数据存放空间 */
    char *p_data;
} zRefData__;

/* 对 struct iovec 的封装，用于 zsendmsg 函数 */
typedef struct __zVecWrap__ {
    /* 此数组中每个成员的 iov_base 字段均指向 p_refData_ 中对应的 p_data 字段 */
    struct iovec *p_vec_;
    _i vecSiz;

    struct __zRefData__ *p_refData_;
} zVecWrap__;

/*
 * 用于存放每个项目的专用元信息
 */
typedef struct {
    /* 
     * 所有需要传递给线程的数据结构
     * 均需在最前面预留一个指针的空间
     */
    zThreadPool__ *p_threadSource_;

    /* 项目 ID */
    _i repoId;

    /*
     * 版本号列表、差异文件列表等缓存的 ID
     * 实质就是每次刷新缓存时的 UNIX 时间戳
     */
    time_t  cacheId;

    /*
     * 项目在服务端上的绝对路径
     */
    char *p_repoPath;

    /*
     * 服务端项目路径字符串长度
     * 会被频繁使用，因此全局留存以提升性能
     */
    _i repoPathLen;

    /*
     * 项目所在文件系统支持的最大路径长度
     * 相对于项目根目录的值，由底层文件系统决定
     * 用于度量 git 输出的差异文件相对路径长度
     */
    _i maxPathLen;

    /*
     * 用于标识仓库是否已经初始化完成：N 代表动作尚未完成，Y 代表已完成
     * 未初始化完成的项目，不接受任何动作请求
     */
    char initFinished;

    /*
     * 新布署请求通过改变此变量的值 (1 ===> 0) 打断旧的布署动作
     */
    _i dpingMark;

    /*
     * 每次布署的开始时间
     * 每台目标机的耗时均基于此计算
     */
    time_t  dpBaseTimeStamp;

    /*
     * 本项目全局 git handler
     */
    git_repository *p_gitRepoHandler;

    /*
     * 本项目全局 postgreSQL handler
     */
    zPgConnHd__ *p_pgConnHd_;

    /*
     * 用于控制并发流量的信号量
     * 防止并发超载
     */
    sem_t dpTraficControl;

    /*
     * 用于任务完成计数的原子性统计
     * 及通知调度线程任务已完成
     */
    pthread_mutex_t dpSyncLock;
    pthread_cond_t dpSyncCond;

    /*
     * 每个布署时指定的目标主机总数
     */
    _i totalHost;

    /*
     * 布署总任务数，其值总是 <= totalHost
     */
    _i dpTotalTask;

    /*
     * 任务完成数：此值与 dpTotalTask 相等时，即代表所有动作已完成
     * 但不代表全部成功，其中可能存在因发生错误而返回的结果
     */
    _i dpTaskFinCnt;

    /*
     * 用于标识本项目是否处于锁定状态
     * 即：可查询，但不允许布署
     */
    _c repoLock;

    /*
     * 代码库状态，若上一次布署失败，此项将置为 zRepoDamaged 状态
     * 用于提示用户看到的信息可能不准确
     */
    _c repoState;

    /*
     * 用于标识收集齐的结果是全部成功，还是其中有异常返回而增加的计数
     * bit[0] 置位表示目标机初始化过程中发生错误
     * bit[1] 置位表示布署过程中发生错误
     */
    _uc resType;

    /* 存放最近一次布署成功的版本号 */
    char lastDpSig[44];

    /* 正在布署过程中但尚未确定最终结果的版本号 */
    char dpingSig[44];

    /* 同一个项目的所有目标机登陆认证方式必须完全相同 */
    char sshUserName[256];
    char sshPort[6];

    /*
     * 存放所有目标机的并发布署参数
     * 目标机数量不超过 zForecastedHostNum 时，使用预置的空间，以提升效率
     */
    zDpCcur__ *p_dpCcur_;
    zDpCcur__ dpCcur_[zForecastedHostNum];

    /*
     * 每次布署时的所有目标机 IP(_ull[2]) 的链表及其散列
     * 用于增量对比差异 IP，并由此决定每台目标机是否需要初始化或布署
     */
    zDpRes__ *p_dpResList_;
    zDpRes__ *p_dpResHash_[zDpHashSiz];

    /*
     * 拿到此锁的线程才有权中止正在进行的布署动作
     * 用于确保同一时间不会有多个中断请求
     */
    pthread_mutex_t dpWaitLock;

    /*
     * 布署主锁：同一项目同一时间只允许一套布署流程在运行
     */
    pthread_mutex_t dpLock;

    /*
     * 布署成功之后，刷新项目缓存时需要此锁
     * 此锁将排斥所有查询类操作
     * 读写锁属性：写者优先
     */
    pthread_rwlock_t rwLock;
    //pthread_rwlockattr_t zRWLockAttr;

    /*
     * 并发布署屏障
     * 用于保证基础环境就绪之后，工作线程才开始真正的布署动作
     * 需要每次布署时根据目标机数量实时初始化，不能随项目启动执行初始化
     */
    //pthread_barrier_t dpBarrier;

    /* 存放新的版本号记录 */
    zVecWrap__ commitVecWrap_;
    struct iovec commitVec_[zCacheSiz];
    zRefData__ commitRefData_[zCacheSiz];

    /* 存放布署记录 */
    zVecWrap__ dpVecWrap_;
    struct iovec dpVec_[zCacheSiz];
    zRefData__ dpRefData_[zCacheSiz];

    /*
     * 线程内存池，预分配 8M 空间
     * 后续以 8M 为步进增长
     */
    void *p_memPool;

    /* 内存池锁，保证内存的原子性分配 */
    pthread_mutex_t memLock;

    /* 动态指示下一次内存分配的起始地址 */
    _ui memPoolOffSet;

    /*
     * 临时 SQL 表命名序号
     * 用以提升布署结果异步查询的性能
     */
    _ui tempTableNo;
} zRepo__;


/*
 * 现存的所有项目 ID 中的最大值
 */
extern _i zGlobMaxRepoId;

/* 系统 CPU 与 MEM 负载监控：以 0-100 表示 */
extern pthread_mutex_t zGlobCommonLock;
extern pthread_cond_t zGlobCommonCond;  // 系统由高负载降至可用范围时，通知等待的线程继续其任务(注：使用全局通用锁与之配套)
extern _ul zGlobMemLoad;  // 高于 80 拒绝布署，同时 git push 的过程中，若高于 80 则剩余任阻塞等待
extern char zGlobPgConnInfo[2048];  // postgreSQL 全局统一连接方式：所有布署相关数据存放于一个数据库中

/* 服务端自身的 IP 地址与端口 */
typedef struct {
    char *p_ipAddr;
    char *p_port;
} zNetSrv__;
extern zNetSrv__ zNetSrv_;

/* 全局所有项目的 META HASH */
extern zRepo__ *zpGlobRepo_[zGlobRepoIdLimit];

typedef struct __zCacheMeta__ {
    _i opsId;  // 网络交互时，代表操作指令（从 1 开始的连续排列的非负整数）
    _i repoId;  // 项目代号（从 1 开始的非负整数）
    _i commitId;  // 版本号
    _i fileId;  // 单个文件在差异文件列表中 index
    _l cacheId;  // 缓存版本代号（最新一次布署的时间戳）
    _i dataType;  // 缓存类型，zIsCommitDataType/zIsDpDataType

    char *p_filePath;  // git diff --name-only 获得的文件路径
    char *p_treeData;  // 经过处理的 Tree 图显示内容

    /* 以下为 Tree 专属数据 */
    struct __zCacheMeta__ *p_father;  // Tree 父节点
    struct __zCacheMeta__ *p_left;  // Tree 左节点
    struct __zCacheMeta__ *p_firstChild;  // Tree 首子节点：父节点唯一直接相连的子节点
    struct __zCacheMeta__ **pp_resHash;  // Tree 按行号对应的散列
    _i lineNum;  // 行号
    _i offSet;  // 纵向偏移
} zCacheMeta__;

struct zDpOps__ {
    _i (* show_dp_process) (cJSON *, _i);

    _i (* print_revs) (cJSON *, _i);
    _i (* print_diff_files) (cJSON *, _i);
    _i (* print_diff_contents) (cJSON *, _i);

    _i (* creat) (cJSON *, _i);

    _i (* dp) (cJSON *, _i);
    _i (* req_dp) (cJSON *, _i);

    _i (* state_confirm) (cJSON *, _i);

    _i (* lock) (cJSON *, _i);
    _i (* unlock) (cJSON *zpJRoot, _i zSd);

    _i (* req_file) (cJSON *, _i);

    void * (* route) (void *);
};


#define zIpVecCmp(zVec0, zVec1) ((zVec0)[0] == (zVec1)[0] && (zVec0)[1] == (zVec1)[1])

#define /*_i*/ zConvert_IpStr_To_Num(/*|_llu [2]|*/ zpIpStr, /*|char *|*/ zpNumVec) ({\
    _i zErrNo = 0;\
    if ('.' == zpIpStr[1] || '.' == zpIpStr[2] || '.' == zpIpStr[3]) {\
        zErrNo = zNetUtils_.to_numaddr(zpIpStr, zIpTypeV4, zpNumVec);\
    } else {\
        zErrNo = zNetUtils_.to_numaddr(zpIpStr, zIpTypeV6, zpNumVec);\
    };\
    zErrNo;  /* 宏返回值 */\
})

#define /*_i*/ zConvert_IpNum_To_Str(/*|_llu [2]|*/ zpNumVec, /*|char *|*/ zpIpStr) ({\
    _i zErrNo = 0;\
    if (0xff == zpNumVec[1] /* IPv4 */) {\
        zErrNo = zNetUtils_.to_straddr(zpNumVec, zIpTypeV4, zpIpStr);\
    } else {\
        zErrNo = zNetUtils_.to_straddr(zpNumVec, zIpTypeV6, zpIpStr);\
    } \
    zErrNo;  /* 宏返回值 */\
})


#endif  //  #ifndef ZDPOPS_H
