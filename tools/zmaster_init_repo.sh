#!/bin/sh

###################
zProjNo=$1  # 项目ID
zPathOnHost=$(printf $2 | sed -n 's%/\+%/%p')  # 生产机上的绝对路径
zPullAddr=$3  # 拉取代码所用的完整地址
zRemoteMasterBranchName=$4  # 源代码服务器上用于对接生产环境的分支名称
zRemoteVcsType=$5  # svn 或 git
###################

zShadowPath=/home/git/zgit_shadow
zDeployPath=/home/git/${zPathOnHost}

if [[ "" == $zProjNo
    || "" == $zDeployPath
    || "" == $zPullAddr
    || "" == $zRemoteMasterBranchName
    || "" == $zRemoteVcsType ]]
then
    exit 1
fi

# 已存在相同路径的情况：若项目路径相同，但ID不同，返回失败
if [[ 0 -lt `ls -d ${zDeployPath} | wc -l` ]]; then
    if [[ $zProjNo -eq `cat ${zDeployPath}_SHADOW/info/repo_id` ]]; then
        cd ${zDeployPath}_SHADOW
        cp -rf ${zShadowPath}/tools ./
        eval sed -i 's%__PROJ_PATH%${zPathOnHost}%g' ./tools/post-update
        exit 0
    else
        exit 255
    fi
fi

# 创建项目路径
mkdir -p $zDeployPath
if [[ $? -ne 0 ]]; then exit 254; fi

# 拉取远程代码
if [[ "svn" == $zRemoteVcsType ]]; then
    svn co $zPullAddr $zDeployPath
else
    git clone $zPullAddr $zDeployPath
fi

if [[ $? -ne 0 ]]; then
    rm -rf $zDeployPath
    exit 253
fi

# 环境初始化
cd $zDeployPath
git init .  # FOR svn
git config user.name "$zProjNo"
git config user.email "${zProjNo}@${zPathOnHost}"
printf ".svn/\n" > .gitignore  # 忽略<.svn>目录
git add --all .
git commit -m "__init__"
git branch -f CURRENT
git branch -f server  # 远程代码接收到server分支

# 创建以 <项目名称>_SHADOW 命名的目录，初始化为git库
mkdir -p ${zDeployPath}_SHADOW/tools
rm -rf ${zDeployPath}_SHADOW/tools/*
cp -r ${zShadowPath}/tools/* ${zDeployPath}_SHADOW/tools/
cd ${zDeployPath}_SHADOW
eval sed -i 's%__PROJ_PATH%${zPathOnHost}%g' ./tools/post-update

git init .
git config user.name "git_shadow"
git config user.email "git_shadow@${zProjNo}"
git add --all .
git commit --allow-empty -m "__init__"

# 防止添加重复条目
zExistMark=`cat /home/git/zgit_shadow/conf/master.conf | grep -Pc "^\s*${zProjNo}\s*"`
if [[ 0 -eq $zExistMark ]];then
    zDirName=`dirname \`dirname ${zPathOnHost}\``
    zBaseName=`basename ${zPathOnHost}`
    printf "${zProjNo} ${zDirName}/${zBaseName}  ${zPullAddr} ${zRemoteMasterBranchName} ${zRemoteVcsType}\n" >> ${zShadowPath}/conf/master.conf
fi

# 创建必要的目录与文件
cd ${zDeployPath}_SHADOW
mkdir -p ${zDeployPath}_SHADOW/{info,log/deploy}
touch ${zDeployPath}_SHADOW/info/repo_id
touch ${zDeployPath}_SHADOW/log/deploy/meta
chmod -R 0755 ${zDeployPath}_SHADOW

# use to get diff when no deploy log has been written
cd ${zDeployPath}
git branch ____base.XXXXXXXX &&\
    (\
        git checkout ____base.XXXXXXXX;\
        \ls -a | grep -Ev '^(\.|\.\.|\.git)$' | xargs rm -rf;\
        git add --all .;\
        git commit --allow-empty -m "_";\
        zOrigLog=`git log -1 --format="%H_%ct"`;\
        git branch ${zOrigLog};\
        printf "${zOrigLog}\n" > ${zDeployPath}_SHADOW/log/deploy/meta;\
        git checkout master\
    )
