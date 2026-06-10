# 如何在 Linux 环境下运行

- 需要在 linux 下安装 nginx 服务，然后直接将本文件夹整个复制成为 /var/www/html/kiloHistory
- 当复制到位之后，直接以用户身份，运行该目录下的 install-kiloHistory.sh 脚本
- 它会使用 8080 端口进行反代，连接 kilo.db 数据库
- 你可以通过 http://ip/kiloHistory 访问 kilo 的所有会话记录

# 如何在 Windows 环境下运行

- 需要在 windows 下安装 IIS 服务，然后直接将本文件夹整个复制成为 C:\inetpub\wwwroot\kiloHistory
- 当复制到位之后，直接以管理员身份，运行该目录下的 kiloHistory-Service.cmd 脚本，按照它的提示安装
- windows 环境下部署此静态网页需要 IIS + Python + nssm
- 如果不想安装额外的 windows 服务，也可以手动执行 Manual-Start-Service.cmd
