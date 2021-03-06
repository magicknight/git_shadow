#include "zrun.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

extern struct zThreadPool__ zThreadPool_;
extern struct zNetUtils__ zNetUtils_;
extern struct zNativeUtils__ zNativeUtils_;
extern struct zNativeOps__ zNativeOps_;
extern struct zDpOps__ zDpOps_;
extern struct zPgSQL__ zPgSQL_;
extern struct zLibGit__ zLibGit_;
extern struct zMd5Sum__ zMd5Sum_;
extern struct zSuperVisor__ zSuperVisor_;

static void zglob_data_config(zArgvInfo__ *zpArgvInfo_);

static void zstart_server(zArgvInfo__ *zpArgvInfo_);
static void * zops_route_tcp_master(void *zp);
static void * zops_route_tcp (void *zp);

static void * zudp_daemon(void *zp);
static void * zops_route_udp (void *zp);

static _i zwrite_log(void *zp,
        _i zSd __attribute__ ((__unused__)),
        struct sockaddr *zpPeerAddr __attribute__ ((__unused__)),
        socklen_t zPeerAddrLen __attribute__ ((__unused__)));

static void zerr_vec_init(void);
static void zserv_vec_init(void);

static _i zhistory_import (cJSON *zpJ, _i zSd);

struct zRun__ zRun_ = {
    .run = zstart_server,
    .write_log = zwrite_log,

    /* mmap in 'main' func */
    .p_sysInfo_ = NULL,
};

#define zUN_PATH_SIZ\
        sizeof(struct sockaddr_un)-((size_t) (& ((struct sockaddr_un*) 0)->sun_path))

/*
 * 项目进程内部分配空间，
 * 主进程中不可见
 */
zRepo__ *zpRepo_ = NULL;

/*
 * 项目并发启动控制
 * 监控数据写入缓冲区控制
 */
static pthread_mutex_t zCommLock = PTHREAD_MUTEX_INITIALIZER;


/* 进程退出时，清理同一进程组的所有进程 */
static void
zexit_clean(void) {
    /*
     * 新建项目时，若新建的子进程异常退出，同样会触发清理动作，
     * 故新建项目时，首先将子进程的 zpRepo_ 置为非 NULL 值
     */
    if (NULL == zpRepo_) {
        kill(0, SIGUSR1);
    }
}

/* 写日志 */
static _i
zwrite_log(void *zp,
        _i zSd __attribute__ ((__unused__)),
        struct sockaddr *zpPeerAddr __attribute__ ((__unused__)),
        socklen_t zPeerAddrLen __attribute__ ((__unused__))) {

    pthread_mutex_lock(zRun_.p_commLock);
    write(zRun_.logFd, zp, 1 + strlen(zp));
    pthread_mutex_unlock(zRun_.p_commLock);

    return 0;
}

/*
 * 服务启动入口
 */
static void
zstart_server(zArgvInfo__ *zpArgvInfo_) {
    /* 必须指定服务端的根路径 */
    if (NULL == zRun_.p_sysInfo_->p_servPath) {
        zPRINT_ERR(0, NULL, "servPath lost!");
        exit(1);
    }

    /* 检查 pgSQL 运行环境是否是线程安全的 */
    if (zFalse == zPgSQL_.thread_safe_check()) {
        zPRINT_ERR(0, NULL, "thread env not safe!");
        exit(1);
    }

    /* 转换为后台守护进程 */
    zNativeUtils_.daemonize(zRun_.p_sysInfo_->p_servPath);

    /* 全局共享数据注册 */
    zglob_data_config(zpArgvInfo_);

    /* 返回的 udp socket 已经做完 bind，若出错，其内部会 exit */
    zRun_.p_sysInfo_->masterSd = zNetUtils_.gen_serv_sd(
            NULL,
            NULL,
            zRun_.p_sysInfo_->unAddrMaster.sun_path,
            zProtoUDP);

    /* 只运行于主进程，负责日志与数据库的写入 */
    zThreadPool_.add(zudp_daemon, & zRun_.p_sysInfo_->masterSd);

    {////
        char _;
        _i zSd = zNetUtils_.gen_serv_sd(NULL, NULL, ".s____", zProtoUDP);

        /* 等待 write_db udp daemon 就緒 */
        while (1) {
            sendto(zSd, "0", zBYTES(1), MSG_NOSIGNAL,
                    (struct sockaddr *) & zRun_.p_sysInfo_->unAddrMaster,
                    zRun_.p_sysInfo_->unAddrLenMaster);

            if (0 < recv(zSd, &_, zBYTES(1), MSG_NOSIGNAL|MSG_DONTWAIT)) {
                break;
            }
        }

        close(zSd);
    }////

    /*
     * 项目库初始化
     * 每个项目对应一个独立的进程
     */
    zNativeOps_.repo_init_all();

    /*
     * 主进程退出时，清理所有项目进程
     * 必须在项目进程启动之后执行，
     * 否则任一项目进程退出，都会触发清理动作
     */
    atexit(zexit_clean);

    /* 监控模块环境初始化 */
    zSuperVisor_.init(NULL);

    /* 返回的 udp socket 已经做完 bind，若出错，其内部会 exit */
    static _i zMonitorSd;
    zMonitorSd = zNetUtils_.gen_serv_sd(
            zRun_.p_sysInfo_->netSrv_.p_ipAddr,
            zRun_.p_sysInfo_->netSrv_.p_port,
            NULL,
            zProtoUDP);

    /* 只运行于主进程，用于目标机监控数据收集 */
    zThreadPool_.add(zudp_daemon, & zMonitorSd);

    /* 只运行于主进程，系统状态监控 */
    zThreadPool_.add(zSuperVisor_.sys_monitor, NULL);

    /*
     * 返回的 socket 已经做完 bind 和 listen
     * 若出错，其内部会 exit
     */
    _i zMajorSd = zNetUtils_.gen_serv_sd(
            zRun_.p_sysInfo_->netSrv_.p_ipAddr,
            zRun_.p_sysInfo_->netSrv_.p_port,
            NULL,
            zProtoTCP);

    /*
     * 会传向新线程，使用静态变量
     * 使用数组防止负载高时造成线程参数混乱
     */
    _i zReqID = 0;
    static _i zSd[256] = {0};
    for (_ui i = 0;; i++) {
        zReqID = i % 256;
        if (-1 == (zSd[zReqID] = accept(zMajorSd, NULL, 0))) {
            zPRINT_ERR_EASY_SYS();
        } else {
            zThreadPool_.add(zops_route_tcp_master, & zSd[zReqID]);
        }
    }
}

/*
 * 主进程路由函数
 */
static void *
zops_route_tcp_master(void *zp) {
    _i zSd = * ((_i *) zp);

    char zDataBuf[16] = {'\0'};
    _i zRepoID = 0;

    /*
     * 必须使用 MSG_PEEK 标志
     * json repoID 字段建议是第一个字段，有助于提高效率：
     * 格式：{"repoID":1,"...":...}
     */
    recv(zSd, zDataBuf, zBYTES(16), MSG_PEEK|MSG_NOSIGNAL);

    /*
     * 若没有提取到数据，结果是 0
     * 项目 ID 范围：1 - (zRun_.p_sysInfo_->globRepoNumLimit - 1)
     * 不允许使用 0
     */
    if (0 == strncmp("{\"repoID\":", zDataBuf, sizeof("{\"repoID\":") - 1)) {
        zRepoID = strtol(zDataBuf + sizeof("{\"repoID\":") - 1, NULL, 10);

        if (0 >= zRepoID
                || zRun_.p_sysInfo_->globRepoNumLimit <= zRepoID) {
            zNetUtils_.send(zSd,
                    "{\"errNo\":-32,\"content\":\"repoID invalid (hint: 1 - 1023)\"}",
                    sizeof("{\"errNo\":-32,\"content\":\"repoID invalid (hint: 1 - 1023)\"}") - 1);
            goto zEndMark;
        }
    } else {
        goto zDirectServ;
    }

    /*
     * 若项目存在且已就绪，将业务 socket 传递给对应的项目进程
     * 若项目存在，但未就绪，提示正在创建过程中
     * 若项目不存在，则收取完整的 json 信息
     */
    if (zRun_.p_sysInfo_->masterPid != zRun_.p_sysInfo_->repoPidVec[zRepoID]) {
        zNetUtils_.send_fd(zRun_.p_sysInfo_->masterSd, zSd,
                & zRun_.p_sysInfo_->unAddrVec_[zRepoID],
                zRun_.p_sysInfo_->unAddrLenVec[zRepoID]);
        goto zEndMark;
    } else {
zDirectServ:;
        char zDataBuf[8192] = {'\0'};
        _i zDataLen = 0,
        zErrNo = 0;

        cJSON *zpJRoot = NULL;
        cJSON *zpJ = NULL;

        recv(zSd, zDataBuf, zBYTES(8192), MSG_PEEK|MSG_NOSIGNAL);

        zpJRoot = cJSON_Parse(zDataBuf);
        zpJ = cJSON_GetObjectItemCaseSensitive(zpJRoot, "opsID");

        if (cJSON_IsNumber(zpJ)) {
            _i zOpsID = zpJ->valueint;

            /*
             * 首字段不是 repoID 的情况，在主进程直接解析
             * 若 opsID 指示的是新建项目、ping-pang 或 请求转输文件，
             * 则直接执行，否则进入常规流程
             */
            switch (zOpsID) {
                case 0:
                case 14:
                case 1:
                case 5:
                    if (0 > (zErrNo = zRun_.p_sysInfo_->ops_tcp[zOpsID](zpJRoot, zSd))) {
                        zDataLen = snprintf(zDataBuf, 8192,
                                "{\"errNo\":%d,\"content\":\"[opsID: %d] %s\"}",
                                zErrNo,
                                zOpsID,
                                zRun_.p_sysInfo_->p_errVec[-1 * zErrNo]);
                        zNetUtils_.send(zSd, zDataBuf, zDataLen);
                    }

                    break;
                default:
                    zpJ = cJSON_GetObjectItemCaseSensitive(zpJRoot, "repoID");
                    if (cJSON_IsNumber(zpJ)) {
                        zRepoID = zpJ->valueint;

                        if (0 >= zRepoID
                                || zRun_.p_sysInfo_->globRepoNumLimit <= zRepoID) {
                            zNetUtils_.send(zSd,
                                    "{\"errNo\":-32,\"content\":\"repoID invalid (hint: 1 - 1023)\"}",
                                    sizeof("{\"errNo\":-32,\"content\":\"repoID invalid (hint: 1 - 1023)\"}") - 1);
                            break;
                        } else {
                            if (zRun_.p_sysInfo_->masterPid != zRun_.p_sysInfo_->repoPidVec[zRepoID]) {
                                zNetUtils_.send_fd(zRun_.p_sysInfo_->masterSd, zSd,
                                        & zRun_.p_sysInfo_->unAddrVec_[zRepoID],
                                        zRun_.p_sysInfo_->unAddrLenVec[zRepoID]);
                                break;
                            }
                        }
                    }

                    zDataLen = snprintf(zDataBuf, 8192,
                            "{\"errNo\":-2,\"content\":\"%s\"}",
                            zRun_.p_sysInfo_->p_errVec[2]);

                    zNetUtils_.send(zSd, zDataBuf, zDataLen);
            };
        } else {
            zDataLen = snprintf(zDataBuf, 8192,
                    "{\"errNo\":-7,\"content\":\"%s\"}",
                    zRun_.p_sysInfo_->p_errVec[7]);

            zNetUtils_.send(zSd, zDataBuf, zDataLen);
        }

        cJSON_Delete(zpJRoot);
        goto zEndMark;
    }

zEndMark:
    close(zSd);
    return NULL;
}

/*
 * 项目进程 tcp 路由函数，用于面向用户的服务
 */
static void *
zops_route_tcp(void *zp) {
    _i zSd = * ((_i *) zp);

    char zDataBuf[4096] = {'\0'};
    char *zpDataBuf = zDataBuf;

    _i zErrNo = 0,
       zOpsID = -1,
       zDataLen = -1,
       zDataBufSiz = 4096;

    /*
     * 若收到的数据量很大，
     * 直接一次性扩展为 1024 倍(4M)的缓冲区
     */
    if (zDataBufSiz == (zDataLen = recv(zSd, zpDataBuf, zDataBufSiz, MSG_NOSIGNAL))) {
        zDataBufSiz *= 1024;
        zMEM_ALLOC(zpDataBuf, char, zDataBufSiz);
        strcpy(zpDataBuf, zDataBuf);
        zDataLen += recv(zSd, zpDataBuf + zDataLen, zDataBufSiz - zDataLen, MSG_NOSIGNAL);
    }

    /* 提取 value[OpsID] */
    cJSON *zpJRoot = cJSON_Parse(zpDataBuf);
    cJSON *zpOpsID = cJSON_GetObjectItemCaseSensitive(zpJRoot, "opsID");

    if (! cJSON_IsNumber(zpOpsID)) {
        zErrNo = -1;
    } else {
        zOpsID = zpOpsID->valueint;

        if (0 > zOpsID
                || zTCP_SERV_HASH_SIZ <= zOpsID
                || NULL == zRun_.p_sysInfo_->ops_tcp[zOpsID]) {
            zErrNo = -1;
        } else if (1 == zOpsID) {
            /* 若项目进程收到新建请求，直接返回错误 */
            zErrNo = -35;
        } else {
            zErrNo = zRun_.p_sysInfo_->ops_tcp[zOpsID](zpJRoot, zSd);
        }
    }

    cJSON_Delete(zpJRoot);

    /*
     * 成功状态及特殊的错误信息在执行函数中直接回复
     * 通用的错误状态返回至此处统一处理
     */
    if (0 > zErrNo) {
        /* 无法解析的数据，打印出其原始信息 */
        if (-1 == zErrNo) {
            // fprintf(stderr, "\342\224\224\342\224\200\342\224\200\033[31;01m[OrigMsg]:\033[00m %s\n", zpDataBuf);
            fprintf(stderr, "\n\033[31;01m[OrigMsg]:\033[00m %s\n", zpDataBuf);
        }

        zDataLen = snprintf(zpDataBuf, zDataBufSiz,
                "{\"errNo\":%d,\"content\":\"[opsID: %d] %s\"}",
                zErrNo,
                zOpsID,
                zRun_.p_sysInfo_->p_errVec[-1 * zErrNo]);
        zNetUtils_.send(zSd, zpDataBuf, zDataLen);
    }

    close(zSd);
    if (zpDataBuf != &(zDataBuf[0])) {
        free(zpDataBuf);
    }

    return NULL;
}

static void *
zudp_daemon(void *zpSd) {
    _i zSd = * ((_i *) zpSd);

    /*
     * 监控数据收集服务
     * 单个消息长度不能超过 509
     * 最后一个字符，留作 '\0'
     */
    struct iovec zVec_ = {
        .iov_len = zBYTES(509),
    };

    char zCmsgBuf[CMSG_SPACE(sizeof(_i))];

    struct msghdr zMsg_ = {
        .msg_iov = &zVec_,
        .msg_iovlen = 1,

        .msg_control = zCmsgBuf,

        .msg_flags = 0,
    };

    static zUdpInfo__ zUdpInfo_[256];
    size_t zLen = 0;
    _ui i = 0;
    _uc zReqID = 0;

    /*
     * 收到的内容会传向新线程，
     * 使用静态变量数组防止负载高时造成线程参数混乱
     */
    for (;; i++) {
        zReqID = i % 256;

        zMsg_.msg_name = & zUdpInfo_[zReqID].peerAddr;

        /*
         * !!! 如下两项 !!!
         * 传入时设置为缓冲区容量大小，
         * recvmsg 成功返回时，会写入实际接收到的数据据长度
         */
        zMsg_.msg_namelen = sizeof(struct sockaddr);
        zMsg_.msg_controllen = CMSG_SPACE(sizeof(_i)),

        /* 常规数据缓冲区 */
        zVec_.iov_base = zUdpInfo_[zReqID].data;

        /*
         * == 0，则表明 zsend_fd() 发送过来的是套接字 sd
         * > 0，则表示传递的是常规数据，而非 sd
         * < 0，表示出错
         */
        if (0 == (zLen = recvmsg(zSd, &zMsg_, MSG_NOSIGNAL))) {
            if (NULL == CMSG_FIRSTHDR(&zMsg_)) {
                zPRINT_ERR_EASY("recv fd err");
                continue;
            } else {
                /*
                 * TCP 套接字进程间传递
                 * 只发送了一个 cmsghdr 结构体 + fd
                 * 其最后的 data[] 存放的即是接收到的 fd
                 */
                /* sentSd 字段段存放接收者向发送者通信所需的句柄 */
                zUdpInfo_[zReqID].sentSd = * (_i *) CMSG_DATA(CMSG_FIRSTHDR(& zMsg_));
                zThreadPool_.add(zops_route_tcp, & zUdpInfo_[zReqID].sentSd);
            }
        } else if (0 < zLen){
            /* 客户端发送的字符串可能不是以 '\0' 结尾 */
            zUdpInfo_[zReqID].data[zLen] = '\0';

            /* sentSd 字段存放接收者向发送者通信所需的句柄 */
            zUdpInfo_[zReqID].sentSd = zSd;
            zUdpInfo_[zReqID].peerAddrLen = zMsg_.msg_namelen;
            zThreadPool_.add(zops_route_udp, & zUdpInfo_[zReqID]);
        } else {
            zPRINT_ERR_EASY("");
            continue;
        }
    }

    return NULL;
}

/*
 * udp 路由函数，用于服务器内部
 * 首字符充当路由索引
 * 0 在 ansi 表中对应值是 48
 * 故，首字符减去 48，即可得到二进制格式的 0-9
 */
static void *
zops_route_udp (void *zp) {
    zUdpInfo__ zUdpInfo_;

    /* 必须第一时间复制出来 */
    memcpy(&zUdpInfo_, zp, sizeof(zUdpInfo__));

    if (47 < zUdpInfo_.data[0]
            && 58 > zUdpInfo_.data[0]
            && NULL != zRun_.p_sysInfo_->ops_udp[zUdpInfo_.data[0] - 48]
            && 0 == zRun_.p_sysInfo_->ops_udp[zUdpInfo_.data[0] - 48](
                zUdpInfo_.data + 1,
                zUdpInfo_.sentSd,
                & zUdpInfo_.peerAddr,
                zUdpInfo_.peerAddrLen)) {
        return NULL;
    } else {
        return (void *) -1;
    }
}


static void
zserv_vec_init(void) {
    /*
     * TCP、UDP 路由函数
     */
    zRun_.p_sysInfo_->route_tcp = zops_route_tcp,
    zRun_.p_sysInfo_->route_udp = zops_route_udp,

    /*
     * TCP serv vec
     * 索引范围：0 至 zTCP_SERV_HASH_SIZ - 1
     */
    zRun_.p_sysInfo_->ops_tcp[0] = zDpOps_.tcp_pang;  /* 目标机使用此接口测试与服务端的连通性 */
    zRun_.p_sysInfo_->ops_tcp[1] = zDpOps_.add_repo;  /* 创建新项目 */
    zRun_.p_sysInfo_->ops_tcp[2] = zDpOps_.del_repo;  /* 删除项目 */
    zRun_.p_sysInfo_->ops_tcp[3] = zDpOps_.repo_update;  /* 源库URL或分支更改 */
    zRun_.p_sysInfo_->ops_tcp[4] = NULL;
    zRun_.p_sysInfo_->ops_tcp[5] = zhistory_import;  /* 临时接口，用于导入旧版系统已产生的数据 */
    zRun_.p_sysInfo_->ops_tcp[6] = NULL;
    zRun_.p_sysInfo_->ops_tcp[7] = zDpOps_.glob_res_confirm;  /* 目标机自身布署成功之后，向服务端核对全局结果，若全局结果是失败，则执行回退 */
    zRun_.p_sysInfo_->ops_tcp[8] = zDpOps_.state_confirm;  /* 远程主机初始经状态、布署结果状态、错误信息 */
    zRun_.p_sysInfo_->ops_tcp[9] = zDpOps_.print_revs;  /* 显示提交记录或布署记录 */
    zRun_.p_sysInfo_->ops_tcp[10] = zDpOps_.print_diff_files;  /* 显示差异文件路径列表 */
    zRun_.p_sysInfo_->ops_tcp[11] = zDpOps_.print_diff_contents;  /* 显示差异文件内容 */
    zRun_.p_sysInfo_->ops_tcp[12] = zDpOps_.dp;  /* 批量布署或撤销 */
    zRun_.p_sysInfo_->ops_tcp[13] = NULL;
    zRun_.p_sysInfo_->ops_tcp[14] = zDpOps_.req_file;  /* 请求服务器发送指定的文件 */
    zRun_.p_sysInfo_->ops_tcp[15] = zDpOps_.show_dp_process;  /* 查询指定项目的详细信息及最近一次的布署进度 */

    /* UDP serv vec */
    zRun_.p_sysInfo_->ops_udp[0] = zDpOps_.udp_pang;
    zRun_.p_sysInfo_->ops_udp[1] = zDpOps_.state_confirm_inner;
    zRun_.p_sysInfo_->ops_udp[2] = NULL;
    zRun_.p_sysInfo_->ops_udp[3] = NULL;
    zRun_.p_sysInfo_->ops_udp[4] = NULL;
    zRun_.p_sysInfo_->ops_udp[5] = NULL;
    zRun_.p_sysInfo_->ops_udp[6] = NULL;
    zRun_.p_sysInfo_->ops_udp[7] = zSuperVisor_.write_db;
    zRun_.p_sysInfo_->ops_udp[8] = zPgSQL_.write_db;
    zRun_.p_sysInfo_->ops_udp[9] = zRun_.write_log;
}

/*
 * 提取必要的基础信息
 * 只需在主进程执行一次，项目进程会继承之
 */
static void
zglob_data_config(zArgvInfo__ *zpArgvInfo_) {
    struct passwd *zpPWD = NULL;
    char zDBPassFilePath[1024];

    struct stat zS_;

    char zPath[zRun_.p_sysInfo_->servPathLen + sizeof("/tools/post-update")];

    /* 项目进程唯一性保证；日志有序性保证 */
    zRun_.p_commLock = &zCommLock;

    /* 主进程 pid */
    zRun_.p_sysInfo_->masterPid = getpid();

    /* 每个项目进程对应的 UNIX domain sd 路径 */
    zRun_.p_sysInfo_->unAddrMaster.sun_family = PF_UNIX;

    zRun_.p_sysInfo_->unAddrLenMaster =
        (size_t) (((struct sockaddr_un *) 0)->sun_path)
        + snprintf(
                zRun_.p_sysInfo_->unAddrMaster.sun_path,
                zUN_PATH_SIZ,
                ".s.master");

    for (_i i =0; i < zGLOB_REPO_NUM_LIMIT; i++) {
        /* 每个项目进程对应的 UNIX domain sd 路径 */
        zRun_.p_sysInfo_->unAddrVec_[i].sun_family = PF_UNIX;

        zRun_.p_sysInfo_->unAddrLenVec[i] =
            (size_t) (((struct sockaddr_un *) 0)->sun_path)
            + snprintf(
                    zRun_.p_sysInfo_->unAddrVec_[i].sun_path,
                    zUN_PATH_SIZ,
                    ".s.%d",
                    i);

        /* 以主进程 pid 的值，预置项目进程 pid */
        zRun_.p_sysInfo_->repoPidVec[i] = zRun_.p_sysInfo_->masterPid;

        /* 预置为 NULL */
        zRun_.p_sysInfo_->pp_repoMetaVec[i] = NULL;
    }

    /* 计算 post-update MD5sum */
    sprintf(zPath, "%s/tools/post-update",
            zRun_.p_sysInfo_->p_servPath);

    zMd5Sum_.md5sum(zPath, zRun_.p_sysInfo_->gitHookMD5);

    /* 打开全局日志文件 */
    sprintf(zPath, "%s/log/log",
            zRun_.p_sysInfo_->p_servPath);

    zCHECK_NEGATIVE_EXIT(
            zRun_.logFd = open(zPath, O_WRONLY|O_CREAT|O_APPEND, 0700)
            );

    zRun_.p_sysInfo_->udp_daemon = zudp_daemon,

    zRun_.p_sysInfo_->globRepoNumLimit = zGLOB_REPO_NUM_LIMIT;

    if (NULL == zRun_.p_sysInfo_->p_loginName) {
        zRun_.p_sysInfo_->p_loginName = "git";
    }

    zCHECK_NULL_EXIT( zpPWD = getpwnam(zRun_.p_sysInfo_->p_loginName) );
    zRun_.p_sysInfo_->p_homePath = zpPWD->pw_dir;
    zRun_.p_sysInfo_->homePathLen = strlen(zRun_.p_sysInfo_->p_homePath);
    zRun_.p_sysInfo_->servPathLen = strlen(zRun_.p_sysInfo_->p_servPath);

    zMEM_ALLOC(zRun_.p_sysInfo_->p_sshPubKeyPath, char, zRun_.p_sysInfo_->homePathLen + sizeof("/.ssh/id_rsa.pub"));
    sprintf(zRun_.p_sysInfo_->p_sshPubKeyPath, "%s/.ssh/id_rsa.pub", zRun_.p_sysInfo_->p_homePath);

    zMEM_ALLOC(zRun_.p_sysInfo_->p_sshPrvKeyPath, char, zRun_.p_sysInfo_->homePathLen + sizeof("/.ssh/id_rsa"));
    sprintf(zRun_.p_sysInfo_->p_sshPrvKeyPath, "%s/.ssh/id_rsa", zRun_.p_sysInfo_->p_homePath);

    /* 确保 pgSQL 密钥文件存在并合法 */
    if (NULL == zpArgvInfo_->p_pgPassFilePath) {
        snprintf(zDBPassFilePath, 1024,
                "%s/.pgpass",
                zRun_.p_sysInfo_->p_homePath);

        zpArgvInfo_->p_pgPassFilePath = zDBPassFilePath;
    }

    zCHECK_NOTZERO_EXIT( stat(zpArgvInfo_->p_pgPassFilePath, &zS_) );

    if (! S_ISREG(zS_.st_mode)) {
        zPRINT_ERR_EASY("");
        exit(1);
    }

    zCHECK_NOTZERO_EXIT( chmod(zpArgvInfo_->p_pgPassFilePath, 00600) );

    /* 生成连接 pgSQL 的元信息 */
    snprintf(zRun_.p_sysInfo_->pgConnInfo, 2048,
            "%s%s "
            "%s%s "
            "%s%s "
            "%s%s "
            "%s%s "
            "%s%s "
            "sslmode=allow "
            "connect_timeout=6",
            NULL == zpArgvInfo_->p_pgAddr ? "host=" : "",
            NULL == zpArgvInfo_->p_pgAddr ? (NULL == zpArgvInfo_->p_pgHost ? zRun_.p_sysInfo_->p_servPath : zpArgvInfo_->p_pgHost) : "",
            NULL == zpArgvInfo_->p_pgAddr ? "" : "hostaddr=",
            NULL == zpArgvInfo_->p_pgAddr ? "" : zpArgvInfo_->p_pgAddr,
            (NULL == zpArgvInfo_->p_pgAddr && NULL == zpArgvInfo_->p_pgHost)? "" : (NULL == zpArgvInfo_->p_pgPort ? "" : "port="),
            (NULL == zpArgvInfo_->p_pgAddr && NULL == zpArgvInfo_->p_pgHost)? "" : (NULL == zpArgvInfo_->p_pgPort ? "" : zpArgvInfo_->p_pgPort),
            "user=",
            NULL == zpArgvInfo_->p_pgUserName ? "git" : zpArgvInfo_->p_pgUserName,
            "passfile=",
            zpArgvInfo_->p_pgPassFilePath,
            "dbname=",
            NULL == zpArgvInfo_->p_pgDBName ? "dpDB": zpArgvInfo_->p_pgDBName);

    /* 初始化 serv_map 与 err_map */
    zserv_vec_init();
    zerr_vec_init();

    /* DB 连接池初始化 */
    zPgSQL_.conn_pool_init();

    /*
     * !!! 必须在初始化项目库之前运行
     * 主进程常备线程数量：32
     * 项目进程常备线程数量：4
     * 系统全局可启动线程数上限 4096
     */
    zThreadPool_.init(32, 4096);
}

static void
zerr_vec_init(void) {
    zRun_.p_sysInfo_->p_errVec[0] = "";
    zRun_.p_sysInfo_->p_errVec[1] = "无法识别或未定义的操作请求";
    zRun_.p_sysInfo_->p_errVec[2] = "项目不存在或正在创建过程中";
    zRun_.p_sysInfo_->p_errVec[3] = "指定的版本号不存在";
    zRun_.p_sysInfo_->p_errVec[4] = "指定的文件 ID 不存在";
    zRun_.p_sysInfo_->p_errVec[5] = "";
    zRun_.p_sysInfo_->p_errVec[6] = "项目被锁定，请解锁后重试";
    zRun_.p_sysInfo_->p_errVec[7] = "服务端接收到的数据无法解析";
    zRun_.p_sysInfo_->p_errVec[8] = "已产生新的布署记录，请刷新页面";
    zRun_.p_sysInfo_->p_errVec[9] = "服务端错误：缓冲区容量不足，无法解析网络数据";
    zRun_.p_sysInfo_->p_errVec[10] = "请求的数据类型错误：非提交记录或布署记录";
    zRun_.p_sysInfo_->p_errVec[11] = "系统忙，请两秒后重试...";
    zRun_.p_sysInfo_->p_errVec[12] = "布署失败";
    zRun_.p_sysInfo_->p_errVec[13] = "正在布署过程中，或上一次布署失败，查看最近一次布署动作的实时进度";
    zRun_.p_sysInfo_->p_errVec[14] = "用户指定的布署后命令执行失败";
    zRun_.p_sysInfo_->p_errVec[15] = "服务端布署前动作出错";
    zRun_.p_sysInfo_->p_errVec[16] = "系统当前负载太高，请稍稍后重试";
    zRun_.p_sysInfo_->p_errVec[17] = "IPnum ====> IPstr 失败";
    zRun_.p_sysInfo_->p_errVec[18] = "IPstr ====> IPnum 失败";
    zRun_.p_sysInfo_->p_errVec[19] = "指定的目标机列表中存在重复 IP";
    zRun_.p_sysInfo_->p_errVec[20] = "";
    zRun_.p_sysInfo_->p_errVec[21] = "";
    zRun_.p_sysInfo_->p_errVec[22] = "";
    zRun_.p_sysInfo_->p_errVec[23] = "部分或全部目标机初始化失败";
    zRun_.p_sysInfo_->p_errVec[24] = "前端没有指明目标机总数";
    zRun_.p_sysInfo_->p_errVec[25] = "";
    zRun_.p_sysInfo_->p_errVec[26] = "";
    zRun_.p_sysInfo_->p_errVec[27] = "";
    zRun_.p_sysInfo_->p_errVec[28] = "指定的目标机总数与实际解析出的数量不一致";
    zRun_.p_sysInfo_->p_errVec[29] = "指定的项目路径不合法";
    zRun_.p_sysInfo_->p_errVec[30] = "指定项目路径不是目录，存在非目录文件与之同名";
    zRun_.p_sysInfo_->p_errVec[31] = "SSHUserName 字段太长(>255 char)";
    zRun_.p_sysInfo_->p_errVec[32] = "指定的项目 ID 不合法(1 - 1023)";
    zRun_.p_sysInfo_->p_errVec[33] = "服务端无法创建指定的项目路径";
    zRun_.p_sysInfo_->p_errVec[34] = "项目信息格式错误：信息不足或存在不合法字段";
    zRun_.p_sysInfo_->p_errVec[35] = "项目 ID 已存在";
    zRun_.p_sysInfo_->p_errVec[36] = "服务端项目路径已存在";
    zRun_.p_sysInfo_->p_errVec[37] = "未指定远程代码库的版本控制系统类型：git";
    zRun_.p_sysInfo_->p_errVec[38] = "";
    zRun_.p_sysInfo_->p_errVec[39] = "SSHPort 字段太长(>5 char)";
    zRun_.p_sysInfo_->p_errVec[40] = "服务端项目路径操作错误";
    zRun_.p_sysInfo_->p_errVec[41] = "服务端 git 库异常";
    zRun_.p_sysInfo_->p_errVec[42] = "git clone 错误";
    zRun_.p_sysInfo_->p_errVec[43] = "git config 错误";
    zRun_.p_sysInfo_->p_errVec[44] = "git branch 错误";
    zRun_.p_sysInfo_->p_errVec[45] = "git add and commit 错误";
    zRun_.p_sysInfo_->p_errVec[46] = "libgit2 初始化错误";
    zRun_.p_sysInfo_->p_errVec[47] = "git rev_walker err";
    zRun_.p_sysInfo_->p_errVec[48] = "";
    zRun_.p_sysInfo_->p_errVec[49] = "指定的源库分支无效/同步失败";
    zRun_.p_sysInfo_->p_errVec[50] = "";
    zRun_.p_sysInfo_->p_errVec[51] = "";
    zRun_.p_sysInfo_->p_errVec[52] = "";
    zRun_.p_sysInfo_->p_errVec[53] = "";
    zRun_.p_sysInfo_->p_errVec[54] = "";
    zRun_.p_sysInfo_->p_errVec[55] = "";
    zRun_.p_sysInfo_->p_errVec[56] = "";
    zRun_.p_sysInfo_->p_errVec[57] = "";
    zRun_.p_sysInfo_->p_errVec[58] = "";
    zRun_.p_sysInfo_->p_errVec[59] = "";
    zRun_.p_sysInfo_->p_errVec[60] = "";
    zRun_.p_sysInfo_->p_errVec[61] = "";
    zRun_.p_sysInfo_->p_errVec[62] = "";
    zRun_.p_sysInfo_->p_errVec[63] = "";
    zRun_.p_sysInfo_->p_errVec[64] = "";
    zRun_.p_sysInfo_->p_errVec[65] = "";
    zRun_.p_sysInfo_->p_errVec[66] = "";
    zRun_.p_sysInfo_->p_errVec[67] = "";
    zRun_.p_sysInfo_->p_errVec[68] = "";
    zRun_.p_sysInfo_->p_errVec[69] = "";
    zRun_.p_sysInfo_->p_errVec[70] = "无内容 或 服务端版本号列表缓存错误";
    zRun_.p_sysInfo_->p_errVec[71] = "无内容 或 服务端差异文件列表缓存错误";
    zRun_.p_sysInfo_->p_errVec[72] = "无内容 或 服务端单个文件的差异内容缓存错误";
    zRun_.p_sysInfo_->p_errVec[73] = "";
    zRun_.p_sysInfo_->p_errVec[74] = "";
    zRun_.p_sysInfo_->p_errVec[75] = "";
    zRun_.p_sysInfo_->p_errVec[76] = "";
    zRun_.p_sysInfo_->p_errVec[77] = "";
    zRun_.p_sysInfo_->p_errVec[78] = "";
    zRun_.p_sysInfo_->p_errVec[79] = "";
    zRun_.p_sysInfo_->p_errVec[80] = "目标机请求下载的文件路径不存在或无权访问";
    zRun_.p_sysInfo_->p_errVec[81] = "同一目标机的同一次布署动作，收到重复的状态确认";
    zRun_.p_sysInfo_->p_errVec[82] = "无法创建 <PATH>_SHADOW/____post-deploy.sh 文件";
    zRun_.p_sysInfo_->p_errVec[83] = "";
    zRun_.p_sysInfo_->p_errVec[84] = "";
    zRun_.p_sysInfo_->p_errVec[85] = "";
    zRun_.p_sysInfo_->p_errVec[86] = "";
    zRun_.p_sysInfo_->p_errVec[87] = "";
    zRun_.p_sysInfo_->p_errVec[88] = "";
    zRun_.p_sysInfo_->p_errVec[89] = "";
    zRun_.p_sysInfo_->p_errVec[90] = "数据库连接失败";
    zRun_.p_sysInfo_->p_errVec[91] = "SQL 命令执行失败";
    zRun_.p_sysInfo_->p_errVec[92] = "SQL 执行结果错误";  /* 发生通常代表存在 BUG */
    zRun_.p_sysInfo_->p_errVec[93] = "";
    zRun_.p_sysInfo_->p_errVec[94] = "";
    zRun_.p_sysInfo_->p_errVec[95] = "";
    zRun_.p_sysInfo_->p_errVec[96] = "";
    zRun_.p_sysInfo_->p_errVec[97] = "";
    zRun_.p_sysInfo_->p_errVec[98] = "";
    zRun_.p_sysInfo_->p_errVec[99] = "";
    zRun_.p_sysInfo_->p_errVec[100] = "";
    zRun_.p_sysInfo_->p_errVec[101] = "目标机返回的信息已失效（过时）";
    zRun_.p_sysInfo_->p_errVec[102] = "目标机 post-update 出错返回";
    zRun_.p_sysInfo_->p_errVec[103] = "收到未知的目标机 IP";
    zRun_.p_sysInfo_->p_errVec[104] = "";
    zRun_.p_sysInfo_->p_errVec[105] = "";
    zRun_.p_sysInfo_->p_errVec[106] = "";
    zRun_.p_sysInfo_->p_errVec[107] = "";
    zRun_.p_sysInfo_->p_errVec[108] = "";
    zRun_.p_sysInfo_->p_errVec[109] = "";
    zRun_.p_sysInfo_->p_errVec[110] = "";
    zRun_.p_sysInfo_->p_errVec[111] = "";
    zRun_.p_sysInfo_->p_errVec[112] = "";
    zRun_.p_sysInfo_->p_errVec[113] = "";
    zRun_.p_sysInfo_->p_errVec[114] = "";
    zRun_.p_sysInfo_->p_errVec[115] = "";
    zRun_.p_sysInfo_->p_errVec[116] = "";
    zRun_.p_sysInfo_->p_errVec[117] = "";
    zRun_.p_sysInfo_->p_errVec[118] = "";
    zRun_.p_sysInfo_->p_errVec[119] = "";
    zRun_.p_sysInfo_->p_errVec[120] = "";
    zRun_.p_sysInfo_->p_errVec[121] = "";
    zRun_.p_sysInfo_->p_errVec[122] = "";
    zRun_.p_sysInfo_->p_errVec[123] = "";
    zRun_.p_sysInfo_->p_errVec[124] = "";
    zRun_.p_sysInfo_->p_errVec[125] = "";
    zRun_.p_sysInfo_->p_errVec[126] = "服务端操作系统错误";
    zRun_.p_sysInfo_->p_errVec[127] = "被新的布署请求打断";
}

/**************************************************************
 * 临时接口，用于导入旧版布署系统的项目信息及已产生的布署日志 *
 **************************************************************/
extern struct zPosixReg__ zPosixReg_;
static _i
zhistory_import (cJSON *zpJ __attribute__ ((__unused__)), _i zSd) {
    char *zpConfPath="/home/git/zgit_shadow2/conf/master.conf";
    char zLogPathBuf[4096];

    char zDataBuf[4096];
    char zSQLBuf[4096];

    FILE *zpH0 = NULL;
    FILE *zpH1 = NULL;

    zCHECK_NULL_EXIT(zpH0 = fopen(zpConfPath, "r"));

    zRegRes__ zR_ = {
        .alloc_fn = NULL
    };

    while (NULL != zNativeUtils_.read_line(zDataBuf, 4096, zpH0)) {
        zPosixReg_.str_split(&zR_, zDataBuf, " ");

        sprintf(zSQLBuf, "INSERT INTO repo_meta "
                "(repo_id,path_on_host,source_url,source_branch,source_vcs_type,need_pull,ssh_user_name,ssh_port) "
                "VALUES ('%s','%s','%s','%s','%c','%c','%s','%s')",
                zR_.pp_rets[0],
                zR_.pp_rets[1],
                zR_.pp_rets[2],
                "master",
                'G',
                'Y',
                "git",
                "22");
        zPgSQL_.exec_once(zRun_.p_sysInfo_->pgConnInfo, zSQLBuf, NULL);

        _i zBaseID = time(NULL) / 86400 + 2;

        sprintf(zSQLBuf,
                "CREATE TABLE IF NOT EXISTS dp_log_%s "
                "PARTITION OF dp_log FOR VALUES IN (%s) "
                "PARTITION BY RANGE (time_stamp);"

                "CREATE TABLE IF NOT EXISTS dp_log_%s_%d "
                "PARTITION OF dp_log_%s FOR VALUES FROM (MINVALUE) TO (%d);",
            zR_.pp_rets[0], zR_.pp_rets[0],
            zR_.pp_rets[0], zBaseID, zR_.pp_rets[0], 86400 * zBaseID);

        zPgSQL_.exec_once(zRun_.p_sysInfo_->pgConnInfo, zSQLBuf, NULL);


        for (_i zID = 0; zID < 10; zID++) {
            sprintf(zSQLBuf,
                    "CREATE TABLE IF NOT EXISTS dp_log_%s_%d "
                    "PARTITION OF dp_log_%s FOR VALUES FROM (%d) TO (%d);",
                    zR_.pp_rets[0], zBaseID + zID + 1, zR_.pp_rets[0], 86400 * (zBaseID + zID), 86400 * (zBaseID + zID + 1));

            zPgSQL_.exec_once(zRun_.p_sysInfo_->pgConnInfo, zSQLBuf, NULL);
        }

        sprintf(zLogPathBuf,
                "/home/git/home/git/.____DpSystem/%s_SHADOW/log/deploy/meta",
                zR_.pp_rets[1] + sizeof("/home/git/") -1);

        if (NULL != (zpH1 = fopen(zLogPathBuf, "r"))) {
            while (NULL != zNativeUtils_.read_line(zDataBuf, 4096, zpH1)) {
                zDataBuf[40] = '\0';
                sprintf(zSQLBuf,
                        "INSERT INTO dp_log (repo_id,dp_id,time_stamp,rev_sig,host_ip,host_res,host_timespent) "
                        "VALUES (%ld,floor(random() * 1000000000),%s,'%s','%s','{1,1,1,1}',floor(1 + random() * 10))",
                        strtol(zR_.pp_rets[0], NULL, 10), zDataBuf + 41,
                        zDataBuf,
                        "::1");
                zPgSQL_.exec_once(zRun_.p_sysInfo_->pgConnInfo, zSQLBuf, NULL);
            }
        }

        zPosixReg_.free_res(&zR_);
    }

    zNetUtils_.send(zSd,
            "==== Import Success ====",
            sizeof("==== Import Success ====") - 1);
    return 0;
}


#undef zUN_PATH_SIZ
