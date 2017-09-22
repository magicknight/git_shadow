#!/bin/sh
# 拉取server分支分代码到master分支；
# 通知中控机已收到代码；
export PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin"
export HOME="/home/git"
export zPathOnHost="__PROJ_PATH"

zProjName=`basename ${zPathOnHost}`
zProjOnLinePath=`dirname \`dirname ${zPathOnHost}\``

######################################################################
# 采取换软链接的方式，避免推送大量代码过程中线上代码出现不一致的情况 #
######################################################################
rm -rf ${zProjOnLinePath}/${zProjName}  # 一次性使用，清理旧项目遗留的文件
rm -rf ${zProjOnLinePath}/${zProjName}_SHADOW  # 一次性使用，清理旧项目遗留的文件
# 临时切换至布署仓库工作区
ln -s ${zPathOnHost} ${zProjOnLinePath}/${zProjName}
rm -rf ${zPathOnHost}_OnLine
mkdir ${zPathOnHost}_OnLine
git clone $zPathOnHost/.git ${zPathOnHost}_OnLine
# 切换回线上仓库工作区
rm -rf ${zProjOnLinePath}/${zProjName}
ln -s ${zPathOnHost}_OnLine ${zProjOnLinePath}/${zProjName}

#######################################
# 弃用！git clone 比直接复制快2倍左右 #
#######################################
# cd $zPathOnHost
# # 首先复制新版本文件
# rm -rf ${zPathOnHost}_OnLineNew
# mkdir ${zPathOnHost}_OnLineNew
# find . -maxdepth 1 | grep -vE '(^|/)(\.|\.git)$' | xargs cp -R -t ${zPathOnHost}_OnLineNew/
# # 然后互换名称，同时后台新线程清除旧文件
# mv ${zPathOnHost}_OnLine ${zPathOnHost}_OnLineOld
# mv ${zPathOnHost}_OnLineNew ${zPathOnHost}_OnLine
# rm -rf ${zPathOnHost}_OnLineOld &
# # 最后重建软链接
# rm -rf ${zProjOnLinePath}/${zProjName}  # 一次性使用，清理旧项目遗留的文件
# rm -rf ${zProjOnLinePath}/${zProjName}_SHADOW  # 一次性使用，清理旧项目遗留的文件
# ln -sf ${zPathOnHost}_OnLine ${zProjOnLinePath}/${zProjName}

# 布署完成之后需要执行的动作：<项目名称.sh>
(cd $zPathOnHost && sh ${zPathOnHost}/____post-deploy.sh) &

# 如下部分用于保障相同 sig 可以连续布署，应对失败重试场景
cd $zPathOnHost
git commit --allow-empty -m "____Auto Commit By Deploy System____"
git push --force ./.git master:server >/dev/null 2>&1

# 更新 post-update
rm ${zPathOnHost}/.git/hooks/post-update
cp ${zPathOnHost}_SHADOW/tools/post-update ${zPathOnHost}/.git/hooks/post-update
chmod 0755 ${zPathOnHost}/.git/hooks/post-update

# 更新开机请求布署自身的脚本，设置为隐藏文件
mv ${zPathOnHost}_SHADOW/tools/____req-deploy.sh /home/git/.____req-deploy.sh