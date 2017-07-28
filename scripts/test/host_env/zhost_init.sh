#!/usr/bin/env sh
zProjName="miaopai"
zCodePath=/home/git/$zProjName

sh -x ../zdemo_update.sh
\rm -rf $zCodePath
mkdir -p $zCodePath
\cp -rf ../../../demo/${zProjName}_shadow ${zCodePath}/.git_shadow

cd $zCodePath

git init .
git config --global user.email "ECS@aliyun.com"
git config --global user.name "ECS"
git add --all .
git commit --allow-empty -m "__ECS_init__"
git branch -m master client # 将master分支名称更改为client
git branch server # 创建server分支

cp ./.git_shadow/scripts/test/host_env/zECS_git_post-update.sh ${zCodePath}/.git/hooks/post-update
chmod u+x ${zCodePath}/.git/hooks/post-update
chown -R git:git /home/git