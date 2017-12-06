#include "zRun.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

extern struct zThreadPool__ zThreadPool_;
extern struct zNetUtils__ zNetUtils_;
extern struct zNativeUtils__ zNativeUtils_;
extern struct zNativeOps__ zNativeOps_;
extern struct zDpOps__ zDpOps_;
extern struct zPgSQL__ zPgSQL_;

static void zstart_server(zNetSrv__ *zpNetSrv_, zPgLogin__ *zpPgLogin_);
static void * zops_route (void *zpParam);

struct zRun__ zRun_ = {
    .run = zstart_server,
    .route = zops_route,
    .ops = { NULL },
};

static char *zpErrVec[128];

void
zerr_vec_init(void) {
    zpErrVec[1] = "无法识别或未定义的操作请求";
    zpErrVec[2] = "项目不存在或正在创建过程中";
    zpErrVec[3] = "指定的版本号不存在";
    zpErrVec[4] = "指定的文件 ID 不存在";
    zpErrVec[5] = "";
    zpErrVec[6] = "项目被锁定，请解锁后重试";
    zpErrVec[7] = "服务端接收到的数据无法解析";
    zpErrVec[8] = "已产生新的布署记录，请刷新页面";
    zpErrVec[9] = "服务端错误：缓冲区容量不足，无法解析网络数据";
    zpErrVec[10] = "请求的数据类型错误：非提交记录或布署记录";
    zpErrVec[11] = "系统忙，请稍后重试...";
    zpErrVec[12] = "布署失败";
    zpErrVec[13] = "上一次布署动作失败，请重试该次布署或布署其它版本号";
    zpErrVec[14] = "";
    zpErrVec[15] = "服务端布署前动作出错";
    zpErrVec[16] = "系统当前负载太高，请稍稍后重试";
    zpErrVec[17] = "IPnum ====> IPstr 失败";
    zpErrVec[18] = "IPstr ====> IPnum 失败";
    zpErrVec[19] = "指定的目标机列表中存在重复 IP";
    zpErrVec[20] = "";
    zpErrVec[21] = "";
    zpErrVec[22] = "";
    zpErrVec[23] = "部分或全部目标机初始化失败";
    zpErrVec[24] = "前端没有指明目标机总数";
    zpErrVec[25] = "";
    zpErrVec[26] = "";
    zpErrVec[27] = "";
    zpErrVec[28] = "指定的目标机总数与实际解析出的数量不一致";
    zpErrVec[29] = "";
    zpErrVec[30] = "项目路径不是目录，存在非目录文件与之同名";
    zpErrVec[31] = "SSHUserName 字段太长(>255 char)";
    zpErrVec[32] = "指定的项目 ID 超限(0 - 1024)";
    zpErrVec[33] = "无法创建指定的项目路径";
    zpErrVec[34] = "项目信息格式错误：信息不足或存在不合法字段";
    zpErrVec[35] = "项目 ID 已存在";
    zpErrVec[36] = "项目路径已存在";
    zpErrVec[37] = "未指定远程代码库的版本控制系统类型：git";
    zpErrVec[38] = "";
    zpErrVec[39] = "SSHPort 字段太长(>5 char)";
    zpErrVec[40] = "";
    zpErrVec[41] = "";
    zpErrVec[42] = "git clone 错误";
    zpErrVec[43] = "git config 错误";
    zpErrVec[44] = "git branch 错误";
    zpErrVec[45] = "git add and commit 错误";
    zpErrVec[46] = "";
    zpErrVec[47] = "";
    zpErrVec[48] = "";
    zpErrVec[49] = "";
    zpErrVec[50] = "";
    zpErrVec[51] = "";
    zpErrVec[52] = "";
    zpErrVec[53] = "";
    zpErrVec[54] = "";
    zpErrVec[55] = "";
    zpErrVec[56] = "";
    zpErrVec[57] = "";
    zpErrVec[58] = "";
    zpErrVec[59] = "";
    zpErrVec[60] = "";
    zpErrVec[61] = "";
    zpErrVec[62] = "";
    zpErrVec[63] = "";
    zpErrVec[64] = "";
    zpErrVec[65] = "";
    zpErrVec[66] = "";
    zpErrVec[67] = "";
    zpErrVec[68] = "";
    zpErrVec[69] = "";
    zpErrVec[70] = "无内容 或 服务端版本号列表缓存错误";
    zpErrVec[71] = "无内容 或 服务端差异文件列表缓存错误";
    zpErrVec[72] = "无内容 或 服务端单个文件的差异内容缓存错误";
    zpErrVec[73] = "";
    zpErrVec[74] = "";
    zpErrVec[75] = "";
    zpErrVec[76] = "";
    zpErrVec[77] = "";
    zpErrVec[78] = "";
    zpErrVec[79] = "";
    zpErrVec[80] = "目标机请求下载的文件路径不存在或无权访问";
    zpErrVec[81] = "同一目标机的同一次布署动作，收到重复的状态确认";
    zpErrVec[82] = "无法创建 <PATH>_SHADOW/____post-deploy.sh 文件";
    zpErrVec[83] = "";
    zpErrVec[84] = "";
    zpErrVec[85] = "";
    zpErrVec[86] = "";
    zpErrVec[87] = "";
    zpErrVec[88] = "";
    zpErrVec[89] = "";
    zpErrVec[90] = "数据库连接失败";
    zpErrVec[91] = "SQL 命令执行失败";
    zpErrVec[92] = "SQL 执行结果错误";  /* 发生通常代表存在 BUG */
    zpErrVec[93] = "";
    zpErrVec[94] = "";
    zpErrVec[95] = "";
    zpErrVec[96] = "";
    zpErrVec[97] = "";
    zpErrVec[98] = "";
    zpErrVec[99] = "";
    zpErrVec[100] = "";
    zpErrVec[101] = "目标机返回的版本号与正在布署的不一致";
    zpErrVec[102] = "目标机 post-update 出错返回";
    zpErrVec[103] = "目标机返回的信息类型无法识别";
    zpErrVec[104] = "";
    zpErrVec[105] = "";
    zpErrVec[106] = "";
    zpErrVec[107] = "";
    zpErrVec[108] = "";
    zpErrVec[109] = "";
    zpErrVec[110] = "";
    zpErrVec[111] = "";
    zpErrVec[112] = "";
    zpErrVec[113] = "";
    zpErrVec[114] = "";
    zpErrVec[115] = "";
    zpErrVec[116] = "";
    zpErrVec[117] = "";
    zpErrVec[118] = "";
    zpErrVec[119] = "";
    zpErrVec[120] = "";
    zpErrVec[121] = "";
    zpErrVec[122] = "";
    zpErrVec[123] = "";
    zpErrVec[124] = "";
    zpErrVec[125] = "";
    zpErrVec[126] = "test_conn";
    zpErrVec[127] = "被新的布署请求打断";
}

/************
 * 网络服务 *
 ************/
void
zexit_clean(void) {
    /*
     * 进程退出时
     * 清理同一进程组的所有进程
     */
    kill(0, SIGUSR1);
}

static void
zstart_server() {
    /* 检查 pgSQL 运行环境是否是线程安全的 */
    if (zFalse == zPgSQL_.thread_safe_check()) {
        zPrint_Err(0, NULL, "==== !!! FATAL !!! ====");
        exit(1);
    }

    /*
     * 提取主程序自身的启动路径，
     * 需要注意控制路径长度不超过 256
     */
    zCheck_Null_Exit( getcwd(zRun_.servPath, 256) );

    /* 转换为守护进程 */
    zNativeUtils_.daemonize("/");

    /*
     * 主服务停止时，需要清理所有影子进程
     * 例如：定时同步远程代码的子进程
     */
    atexit(zexit_clean);

    /* 初始化错误信息HashMap */
    zerr_vec_init();

    /* 提取用户名称 */
    if (NULL == zRun_.p_loginName) {
        zRun_.p_loginName = "git";
    }

    /* 提取 $HOME */
    struct passwd *zpPWD = getpwnam(zRun_.p_loginName);
    zCheck_Null_Exit(zRun_.p_homePath = zpPWD->pw_dir);

    zRun_.homePathLen = strlen(zRun_.p_homePath);

    zMem_Alloc(zRun_.p_SSHPubKeyPath, char, zRun_.homePathLen + sizeof("/.ssh/id_rsa.pub"));
    sprintf(zRun_.p_SSHPubKeyPath, "%s/.ssh/id_rsa.pub", zRun_.p_homePath);

    zMem_Alloc(zRun_.p_SSHPrvKeyPath, char, zRun_.homePathLen + sizeof("/.ssh/id_rsa"));
    sprintf(zRun_.p_SSHPrvKeyPath, "%s/.ssh/id_rsa", zRun_.p_homePath);

    /* 线程池初始化 */
    zThreadPool_.init();

    /* 扫描所有项目库并初始化之 */
    zNativeOps_.proj_init_all(& (zRun_.pgLogin_));

    /* 定时扩展新的 pgSQL 日志分区表及清理旧的分区表 */
    zThreadPool_.add(zNativeOps_.extend_pg_partition, NULL);

    /* 索引范围：0 至 zServHashSiz - 1 */
    zRun_.ops[0] = zDpOps_.pang;  /* 目标机使用此接口测试与服务端的连通性 */
    zRun_.ops[1] = zDpOps_.creat;  /* 创建新项目 */
    zRun_.ops[2] = zDpOps_.lock;  /* 锁定某个项目的布署、撤销功能，仅提供查询服务（即只读服务） */
    zRun_.ops[3] = zDpOps_.unlock;  /* 恢复布署、撤销功能 */
    zRun_.ops[4] = NULL;
    zRun_.ops[5] = NULL;
    zRun_.ops[6] = NULL;
    zRun_.ops[7] = zDpOps_.glob_res_confirm;  /* 目标机自身布署成功之后，向服务端核对全局结果，若全局结果是失败，则执行回退 */
    zRun_.ops[8] = zDpOps_.state_confirm;  /* 远程主机初始经状态、布署结果状态、错误信息 */
    zRun_.ops[9] = zDpOps_.print_revs;  /* 显示提交记录或布署记录 */
    zRun_.ops[10] = zDpOps_.print_diff_files;  /* 显示差异文件路径列表 */
    zRun_.ops[11] = zDpOps_.print_diff_contents;  /* 显示差异文件内容 */
    zRun_.ops[12] = zDpOps_.dp;  /* 批量布署或撤销 */
    zRun_.ops[13] = zDpOps_.req_dp;  /* 目标机主协要求同步或布署到正式环境外(如测试用途)的目标机上 */
    zRun_.ops[14] = zDpOps_.req_file;  /* 请求服务器发送指定的文件 */
    zRun_.ops[15] = zDpOps_.show_dp_process;  /* 查询指定项目的详细信息及最近一次的布署进度 */

    /* 返回的 socket 已经做完 bind 和 listen */
    _i zMajorSd = zNetUtils_.gen_serv_sd(zRun_.netSrv_.p_ipAddr, zRun_.netSrv_.p_port, zProtoTcp);

    /* 会传向新线程，使用静态变量；使用数组防止负载高时造成线程参数混乱 */
    static _i zSd[64] = {0};
    for (_ui zCnter = 0;; zCnter++) {
        if (-1 == (zSd[zCnter % 64] = accept(zMajorSd, NULL, 0))) {
            zPrint_Err(errno, "-1 == accept(...)", NULL);
        } else {
            zThreadPool_.add(zops_route, & (zSd[zCnter % 64]));
        }
    }
}


/*
 * 路由函数
 */
static void *
zops_route(void *zpParam) {
    _i zSd = * ((_i *) zpParam);

    char zDataBuf[zGlobCommonBufSiz] = {'\0'};
    char *zpDataBuf = zDataBuf;

    _i zErrNo = 0,
       zOpsId = -1,
       zDataLen = -1,
       zDataBufSiz = zGlobCommonBufSiz;

    /* 若收到的数据量很大，直接一次性扩展为 1024 倍的缓冲区，以简化逻辑 */
    if (zDataBufSiz == (zDataLen = recv(zSd, zpDataBuf, zDataBufSiz, 0))) {
        zDataBufSiz *= 1024;
        zMem_Alloc(zpDataBuf, char, zDataBufSiz);
        strcpy(zpDataBuf, zDataBuf);
        zDataLen += recv(zSd, zpDataBuf + zDataLen, zDataBufSiz - zDataLen, 0);
    }

    /* 最短的json字符串：{"a":}，6 字节 */
    if (zBytes(6) > zDataLen) {
        zPrint_Err(errno, NULL, "recvd data too short(< 6bytes)");
        goto zMarkEnd;
    }

    /* 提取 value[OpsId] */
    cJSON *zpJRoot = cJSON_Parse(zpDataBuf);
    cJSON *zpOpsId = cJSON_GetObjectItemCaseSensitive(zpJRoot, "OpsId");
    if (cJSON_IsNumber(zpOpsId)) {
        zOpsId = zpOpsId->valueint;

        /* 检验 value[OpsId] 合法性 */
        if (0 > zOpsId || zServHashSiz <= zOpsId || NULL == zRun_.ops[zOpsId]) {
            zErrNo = -1;
        } else {
            zErrNo = zRun_.ops[zOpsId](zpJRoot, zSd);
        }
    } else {
        zErrNo = -1;
    }
    cJSON_Delete(zpJRoot);

    /* 成功状态在下层函数中回复，通用的错误状态返回至此处统一处理 */
    if (0 > zErrNo) {
        if (-1 == zErrNo) {
            fprintf(stderr, "\033[31;01m[OrigMsg]:\033[00m %s\n\342\224\224\342\224\200\342\224\200", zpDataBuf);
        }

        zDataLen = snprintf(zpDataBuf, zDataBufSiz, "{\"ErrNo\":%d,\"content\":\"[OpsId: %d] %s\"}", zErrNo, zOpsId, zpErrVec[-1 * zErrNo]);
        zNetUtils_.send_nosignal(zSd, zpDataBuf, zDataLen);

        /*
         * [DEBUG]: 错误信息，打印出一份，
         * 防止客户端已断开的场景导致错误信息丢失
         */
        zPrint_Err(0, NULL, zpDataBuf);
    }

zMarkEnd:
    close(zSd);
    if (zpDataBuf != &(zDataBuf[0])) {
        free(zpDataBuf);
    }
    return NULL;
}
