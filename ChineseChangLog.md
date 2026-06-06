
# 关于如何修改全局配置

全局配置文档在路径: ~/.config/kilo/kilo.jsonc
需要在这个里面添加如下配置即可：

  "bwrap": {
    "tmpfs": ["/tmp", "/root", "/var", "/opt", "/mnt", "/media", "/run", "/srv", "/boot"],
    "symlink": [
      {"from": "usr/bin", "to": "/bin"},
      {"from": "usr/lib", "to": "/lib"},
      {"from": "usr/lib64", "to": "/lib64"},
      {"from": "usr/sbin", "to": "/sbin"}
    ],
    "ro_bind": ["/usr", "/etc", "/home/username/.local/share/kilo"],
    "rw_bind": ["/home/username/.kilo"]
  }

上面的 /home/username/.local/share/kilo 是存放 kilo.db 的地方需要允许只读的权限
上面的 /home/username/.kilo 是存放 kilo 自己开发和安装skill 的位置，需要读写权限

# 关于 linux 下沙箱如何其作用的说明

在使用 ai 进行辅助开发的过程中，总是会发现，ai 一旦具备了权限，它可以肆意的阅读和修改超出明显合理边界的文件
对当前项目之外的部分，产生严重的影响，而且它修改错误，也不会负责，于是我在 ai 调用常用工具的时候进行了代码层面的物理限制
比如，它调用 write 和 edit 工具的时候，需要传递具体修改的文件路径作为参数，在 kilo 插件代为执行这些操作之前
会对该文件路径进行辨别，如果该文件路径不是在当前正在打开的项目以内，直接禁止该操作，并反馈给 ai

除了上面的两个工具之外，ai 还会申请调用 bash 工具，在 bash 工具里面，它可以不受限制的执行任何的的操作
于是这个时候，就需要使用沙箱，经过权衡，我选择了 bwrap 沙箱工具，它的调用时机，就是在每一次 ai 申请 bash 调用的时候
上面的配置文件，就是规定了凡是被设定为 tmpfs 的路径，在 ai 和 kilo 插件本身看来都是空目录
设置为 ro_bind 的只读目录，类似于挂载效果，rw_bind 为可读写的目录

以上的限制，可以在代码层级，最大限度的 ai 操作范围，且允许它执行诸如 find, mv, touch, echo 等正常合规命令

# 添加了 extraSkills 文件夹

这个里面是几个比较好用的 skill，在安装插件的时候，并不会被默认包含
你需要将该目录下所有 skills 直接复制到 /home/username/.kilo/skills/ 目录下即可
