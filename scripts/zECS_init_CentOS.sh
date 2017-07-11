#!/usr/bin/env sh
zProjName="miaopai"
zCodePath=/home/git/$zProjName
zEcsAddrListPath=$zCodePath/.git_shadow/info/client_ip_all.txt #store all ECSs' private IPs
zEcsAddrMajorListPath=$zCodePath/.git_shadow/info/client_ip_major.txt #store all major ECSs' public IPs
zSshKeyPath=$zCodePath/.git_shadow/info/authorized_keys  #store Control Host and major ECSs' SSH pubkeys
zSshKnownHostPath=$zCodePath/.git_shadow/info/known_hosts

#yes|yum install git

#Init git env
#useradd -m -s `which sh` git
#su git -c "yes|ssh-keygen -t rsa -P '' -f /home/git/.ssh/id_rsa"
\rm -rf $zCodePath
\cp -rf ../demo/$zProjName $zCodePath

mkdir -p $zCodePath/.git_shadow
\cp -rf ../demo/${zProjName}_shadow $zCodePath/.git_shadow
touch $zSshKeyPath
chmod 0600 $zSshKeyPath

cd $zCodePath
git init .
git config --global user.email "ECS@aliyun.com"
git config --global user.name "ECS"
git add --all .
git commit -m "INIT"
git branch -m master client # 将master分支名称更改为client
git branch server # 创建server分支

# config git hook
# 拉取server分支分代码到client分支；通知中控机已收到代码；判断自身是否是ECS分发节点，如果是，则向同一项目下的所有其它ECS推送最新收到的代码
printf "#!/bin/sh
export PATH=\"/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin\" &&
export HOME=\"/home/git\" &&

git pull --force $zCodePath/.git server:client &&
#$zCodePath/.git_shadow/bin/git_shadow -C -h 10.30.2.126 -p 20000 &&

chmod 0600 $zSshKeyPath &&
cp -up $zSshKeyPath /home/git/.ssh/ &&
chmod 0600 $zSshKnownHostPath &&
cp -up $zSshKnownHostPath /home/git/.ssh/ &&

for zAddr in \$(ip addr | grep -oP \'(\\d+\\.){3}\\d+(?=/\\d+)\' | grep -v \'^127.0.0\')
do
    if [[ 0 -lt \$(cat $zEcsAddrMajorListPath | grep -c \$zAddr) ]]; then
        zEcsAddrList=\$(cat $zEcsAddrListPath | tr \'\\\n\' \' \')
        for zEcsAddr in \$zEcsAddrList
        do
            git push --force git@\${zEcsAddr}:${zCodePath}/.git client:server &
        done
        break
    fi
done
" > $zCodePath/.git/hooks/post-receive

chmod u+x $zCodePath/.git/hooks/post-receive
chown -R git:git /home/git
