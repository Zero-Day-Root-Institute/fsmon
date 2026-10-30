# fsmod

[English](README.md) | [简体中文](README.zh-CN.md)

fsmod 是一个使用 C 编写的文件系统监控工具，基于 [NowSecure/fsmon](https://github.com/nowsecure/fsmon) 改造。当前版本为 **1.0.0**，支持交互式监控、守护运行、HTTP 上报，以及运行期间临时暂停监控输出和上报。

本文对应当前仓库实现。英文说明见 [README.md](README.md)，iOS 打包说明见 [dist/ios/README.md](dist/ios/README.md)，OpenResty 接收端说明见 [server/openresty/README.md](server/openresty/README.md)。

## 平台与实现

| 平台 | 守护模式默认后端 | HTTP 上报 | 说明 |
| --- | --- | --- | --- |
| iOS 越狱设备 | `devfsev` | 系统 CFNetwork | Rootful、Rootless 两种安装布局，进程均需足够权限及 `/dev/fsevents` 访问能力。 |
| macOS | `fsevapi` | 进程内 libcurl | 可选 `devfsev`；私有内核接口能否使用取决于系统与权限。 |
| Linux | `inotify` | 进程内 libcurl | 可选 `fanotify`，需要相应权限；当前主要报告内容修改/可写关闭，不具备 inotify 完整的新建、删除、重命名覆盖。 |
| Android | `inotify` | 默认构建不启用 | 可提供目标平台 libcurl，选择启用守护上报。 |
| watchOS | 交互式后端 | 默认不启用 | 保留交互式监控。 |

运行时不执行 `curl` 命令。macOS/Linux 使用 libcurl 库，iOS 使用 CoreFoundation/CFNetwork。`kdebug`、`kqueue` 等历史后端只用于交互式功能，不支持当前配置化守护上报。

## 架构、流程与生命周期

| 文件 | 职责 |
| --- | --- |
| `main.c` | 入口、参数解析、后端选择、事件回调和各模块生命周期。 |
| `config.c` / `config.h` | 配置解析、路径规范化、路径/事件/进程/UID/GID/PPID 过滤。 |
| `backend/` | 各平台事件采集和统一事件转换，Apple 私有接口实现位于 `backend/devfsev.c`。 |
| `service.c` | 守护化、PID 文件锁、启动确认和进程停止。 |
| `control.c` | 本地控制 socket、暂停状态、TTL、状态恢复和 `ctl run`。 |
| `report.c` | 主机元信息、JSON 编码、有界内存队列、HTTP 发送、重试和退出排空。 |
| `dist/ios/` | Rootful/Rootless 配置、launchd 模板和 deb 打包脚本。 |
| `server/openresty/` | 接收 JSON 上报并写入 Redis DB 3。 |

主要事件流程：

```text
操作系统事件
    → 后端解析并生成统一事件
    → 路径、事件类型、进程名、UID、GID、PPID 过滤
    → 运行时暂停检查
    ├─→ 可选 JSON 事件日志
    └─→ 有界内存队列 → 后台 HTTP 发送与重试
                    → OpenResty → Redis DB 3 的 fsmod:events
```

启动时先加载并检查配置，处理后台运行和 PID 文件锁，再初始化上报线程、控制接口及监控后端。设备标识、系统版本及内核版本在开始采集前缓存。无参数运行会读取默认配置，保持前台运行，便于 launchd/systemd 接管。配置的监控路径全部不存在时可以启动为空闲服务，不会退回监控整个文件系统。

收到 `SIGTERM`/`SIGINT` 后停止采集并关闭后端，停止控制接口，再按 `shutdown_timeout_seconds` 尝试发送队列中剩余事件，到期取消活动请求并丢弃剩余事件，最后释放线程、PID 锁等资源。该超时是清理目标，底层资源清理可能使退出稍晚。PID 文件锁会保持至清理完成；服务管理器根据自身策略决定是否重启。

### Apple 文件监控

`devfsev` 打开 `/dev/fsevents`，通过 `FSEVENTS_CLONE` 创建私有事件流描述符，再通过 `FSEVENTS_WANT_EXTENDED_INFO` 请求扩展信息。它读取二进制事件流，按参数类型与长度解析路径、PID、文件属性、UID/GID、inode 和内核时间戳，再查询可用的进程名及直接父 PID。内核队列深度为 4096，读取缓冲区为 64 KiB；这两个值是编译期设置，与 HTTP 队列容量无关。

macOS 的 `fsevapi` 使用 FSEventStream 文件事件，通知可能合并，通常没有来源进程、PID/PPID 或成对重命名目标。`devfsev` 的可用性依赖内核、权限和越狱环境；成功编译不等同于目标设备兼容。

监控的是文件系统变化。普通 `fopen`/`fread`/`fwrite` 复制一般只能看到目标文件的新建、内容和属性变化，无法得到读取源文件与目标文件的对应关系。内核实际发出 `FSE_CLONE` 时，才可通过 `filename` 与 `newfile` 得到明确的源/目标路径；fsmod 不推测普通复制的来源。

## iOS 安装与启动

包标识与 launchd 服务名均为 `com.apple.fsmod`。Rootless 指安装前缀，守护进程仍以 root 运行。

| 项目 | Rootful（有根） | Rootless（无根） |
| --- | --- | --- |
| deb 架构标识 | `iphoneos-arm` | `iphoneos-arm64` |
| 可执行文件 | `/usr/sbin/fsmod` | `/var/jb/usr/sbin/fsmod` |
| 默认配置 | `/etc/fsmod.conf` | `/var/jb/etc/fsmod.conf` |
| 自启文件 | `/Library/LaunchDaemons/com.apple.fsmod.plist` | `/var/jb/Library/LaunchDaemons/com.apple.fsmod.plist` |
| PID 文件 | `/var/run/fsmod.pid` | `/var/run/fsmod.pid` |
| 日志文件 | `/var/mobile/Library/Logs/fsmod/fsmod.log` | `/var/mobile/Library/Logs/fsmod/fsmod.log` |

两种包均包含 iOS 12.0 及以上物理设备使用的 arm64 可执行文件，不支持 32 位设备。两种包是替代关系，同一设备只安装符合其越狱布局的一种。不创建 `/usr/bin/fsmod` 兼容入口。

安装对应 deb：

```sh
# Rootful
dpkg -i com.apple.fsmod_1.0.0_iphoneos-arm.deb

# Rootless
dpkg -i com.apple.fsmod_1.0.0_iphoneos-arm64.deb
```

安装脚本检查配置后自动启用并加载服务，自启模板使用 `RunAtLoad=true` 和 `KeepAlive=true`。配置属于 Debian conffile，升级时可以保留本地修改；保留旧配置时，新示例不会自动替换设备上实际生效的文件。配置无效会使安装配置步骤失败，修正后执行 `dpkg --configure com.apple.fsmod`。

以 Rootful 为例检查并重启：

```sh
/usr/sbin/fsmod -T
launchctl kickstart -k system/com.apple.fsmod
launchctl print system/com.apple.fsmod
tail -f /var/mobile/Library/Logs/fsmod/fsmod.log
```

Rootless 将上述 fsmod 命令改为 `/var/jb/usr/sbin/fsmod`。若服务尚未加载：

```sh
# Rootful
launchctl enable system/com.apple.fsmod
launchctl bootstrap system /Library/LaunchDaemons/com.apple.fsmod.plist

# Rootless
launchctl enable system/com.apple.fsmod
launchctl bootstrap system /var/jb/Library/LaunchDaemons/com.apple.fsmod.plist
```

直接前台启动只需 `/usr/sbin/fsmod`，Rootless 为 `/var/jb/usr/sbin/fsmod`。已由 launchd 管理时不要再手动运行第二个实例。修改静态配置后需重启，不支持 SIGHUP 热重载。`launchctl kickstart` 只能重启已加载的服务。

Rootless 只改变越狱安装位置，用户数据仍位于 `/private/var/...`，不是 `/var/jb/private/var/...`。

## 配置文件

桌面模板为 [fsmod.conf.example](fsmod.conf.example)，iOS 模板分别为 [Rootful](dist/ios/fsmod.rootful.conf.example) 和 [Rootless](dist/ios/fsmod.rootless.conf.example)。两个 iOS 文件对应不同安装布局，可独立维护；实际生效的是设备上的 `fsmod.conf`。

格式为 UTF-8 文本，每行 `key=value`。空行和以 `#` 开头的行忽略，值不加引号，首尾空白会去除，值内部的空格与 `#` 保留为字面值。未知键、重复标量键会报错，多值项应重复写多行，不使用逗号分隔路径或 ID。布尔值使用 `true`/`false`。

Rootful 最小示例：

```ini
report_url=http://119.8.59.179:8085/events
# report_token=your-bearer-token

watch_path=/Library
watch_path=/Library/dpkg/info
exclude_path=/Library/dpkg

backend=devfsev
daemonize=false
pid_file=/var/run/fsmod.pid
log_file=/var/mobile/Library/Logs/fsmod/fsmod.log
log_events=false
queue_capacity=1024
report_timeout_seconds=10
report_retries=3
retry_delay_seconds=2
shutdown_timeout_seconds=15
```

`report_url` 必须是设备能访问的地址；手机上的 `127.0.0.1` 表示手机自身。包含 token 的配置建议由 root 所有、权限 `0600`。

### 全部配置项

| 配置项 | 含义与默认值 |
| --- | --- |
| `report_url` | 必填 HTTP/HTTPS 上报地址，每个事件单独 POST。HTTPS 校验证书，不跟随重定向。 |
| `report_token` | 可选 Bearer token，作为 `Authorization: Bearer ...` 请求头发送。 |
| `watch_path` | 必须至少配置一项绝对文件/目录路径；可重复，最多 64 项，包含被跳过的不存在路径。 |
| `watch_events` | 紧前最近一项 `watch_path` 的事件选择，每条路径最多一次；每个新路径默认 `all`。 |
| `exclude_path` | 可重复，排除绝对文件/目录路径；目录覆盖其子树，更深层 watch 可重新纳入。启动时仍要求排除路径存在。 |
| `exclude_process` | 可重复，精确、区分大小写地匹配 `data.proc`，匹配后不写事件日志或上报。默认无。 |
| `watch_uid` | 可重复，事件 UID 白名单，十进制 `0..4294967295`；默认不限制 UID。 |
| `exclude_uid` | 可重复，事件 UID 排除名单，范围同上，与白名单冲突时排除优先。 |
| `watch_gid` | 可重复，事件 GID 白名单，十进制 `0..4294967295`；默认不限制 GID。 |
| `exclude_gid` | 可重复，事件 GID 排除名单，范围同上，与白名单冲突时排除优先。 |
| `watch_ppid` | 可重复，操作进程直接父 PID 白名单，十进制 `1..2147483647`；默认不限制 PPID。 |
| `exclude_ppid` | 可重复，直接父 PID 排除名单，范围同上，与白名单冲突时排除优先。 |
| `backend` | iOS 默认 `devfsev`，macOS 默认 `fsevapi`，Linux 默认 `inotify`。 |
| `daemonize` | 桌面配置默认 `true`，iOS 默认 `false`。无参数/隐式默认配置启动保持前台；显式 `-C` 的桌面启动才按此项选择。`-F` 强制前台，桌面 `-D` 强制后台。iOS 始终前台，显式 `-D` 报错。 |
| `pid_file` | 默认 `/tmp/fsmod.pid`，绝对路径，父目录必须存在。进程生命周期内保持文件锁。 |
| `log_file` | 默认 `/tmp/fsmod.log`，绝对路径，父目录必须存在，不能与 PID 文件相同。后台模式 stdout/stderr 追加写入这里；前台由调用者或服务管理器接管输出。 |
| `log_events` | 默认 `false`；`true` 时把通过过滤的事件以 JSON 写至 stdout。生命周期、告警和控制日志不受此项关闭。 |
| `queue_capacity` | HTTP 待发送事件容量，默认 `1024`，范围 `1..1000000`，队列满时丢弃新事件并计数。 |
| `report_timeout_seconds` | 每次连接/传输尝试的超时，默认 `10` 秒，范围 `1..3600`。 |
| `report_retries` | 初次请求失败后的额外重试次数，默认 `3`，范围 `0..100`，即默认最多尝试 4 次。 |
| `retry_delay_seconds` | 重试间隔，默认 `2` 秒，范围 `0..3600`。 |
| `shutdown_timeout_seconds` | 停止时队列排空/取消目标，默认 `15` 秒，范围 `1..3600`；到期后丢弃剩余事件。 |

除 `watch_path` 最多 64 项外，`exclude_path`、`exclude_process` 和各 ID 名单没有独立固定条数上限，受内存等资源限制。单行最长 8192 字节。进程名和 ID 列表会去重。

### 路径匹配与例外

路径必须是绝对路径，存在的路径通过 `realpath` 规范化。允许父子 watch，拒绝规范化后完全相同的 watch（包括指向同一路径的符号链接）。目录规则按路径组件匹配整个子树，`/Library/dpkg` 不匹配 `/Library/dpkg-backup`。

**更具体的路径优先，与配置顺序无关；路径相同时排除优先。**

```ini
watch_path=/Library
watch_path=/Library/dpkg/info
exclude_path=/Library/dpkg
```

| 事件路径 | 结果 |
| --- | --- |
| `/Library/example.txt` | 监控。 |
| `/Library/dpkg/status` | 排除。 |
| `/Library/dpkg/info/example.list` | 更深的 watch 恢复监控。 |
| `/Library/dpkg/info/subdir/example` | 恢复监控，同样覆盖子目录。 |

还可用更深的 `exclude_path` 排除恢复区域中的一部分。路径通过并不绕过事件类型、进程名和 ID 等其他过滤。

`watch_path` 不存在或某个父路径不是目录（`ENOENT`/`ENOTDIR`）时仅警告并跳过，不影响其他路径。其他路径错误仍导致检查或启动失败。所有 watch 都被跳过时服务以 `roots=0` 空闲运行；之后新建路径不会自动加入，需重启。所有有效 watch 都被最终排除时配置检查失败。缺失的 `exclude_path` 仍是错误。

配置中所有逻辑规则都会保留；采集通常合并为最外层不重叠根，避免重复采集。Linux `fanotify` 使用挂载点标记，保留显式配置的子根以覆盖嵌套挂载点。文件路径通过监控其父目录并过滤文件名实现，可观察同名文件的原子替换。符号链接在启动时解析一次，不自动跟随之后的重新指向；Linux 不递归跟随目录中的符号链接目录。

对于 rename/clone 等双路径事件，两侧独立计算最终规则，任一侧最终被排除就丢弃整个事件；不在任何规则范围内的一侧不算被排除。否则至少一侧被纳入且允许该事件类型才通过。

### 每个路径选择事件

```ini
watch_path=/private/etc
watch_events=create,modify

watch_path=/bin
watch_events=all
```

`watch_events` 属于其前面最近的 `watch_path`，中间出现注释或其他选项也不会改变归属。每个新 watch 都重新默认为 `all`。父子 watch 同时匹配时，仅使用最具体 watch 的事件选择，不继承、不合并父规则。

| 分组 | 对应事件 |
| --- | --- |
| `all` | 后端提供的全部事件，必须单独使用。 |
| `create` | `FSE_CREATE_FILE`、`FSE_CREATE_DIR`。 |
| `modify` | `FSE_CONTENT_MODIFIED`、`FSE_STAT_CHANGED`、`FSE_CHOWN`、`FSE_XATTR_MODIFIED`、`FSE_XATTR_REMOVED`、`FSE_FINDER_INFO_CHANGED`。 |
| `delete` | `FSE_DELETE`。 |
| `rename` | `FSE_RENAME`。 |
| `clone` | `FSE_CLONE`。 |
| `exchange` | `FSE_EXCHANGE`。 |

分组与准确 `FSE_*` 名称可混用，例如 `create,FSE_CONTENT_MODIFIED`。其他可用准确名称包括 `FSE_DOCID_CREATED`、`FSE_DOCID_CHANGED`、`FSE_UNMOUNT_PENDING`、`FSE_OPEN`、`FSE_CLOSE`、`FSE_CLOSE_WRITABLE`、`FSE_UNKNOWN`。名称区分大小写。未知项、空项、末尾逗号、混用 `all`、没有前置 watch 或重复给同一 watch 配置事件均报错。重复 token 无害。

这些设置只过滤 HTTP 和事件日志，不改变内核订阅。`all` 也只能接受后端已有事件，不能启用读取跟踪或恢复普通复制的源文件。

### 进程、UID、GID 与 PPID

```ini
# 每行一个精确进程名；不自动排除子进程。
exclude_process=zsh
exclude_process=sshd

# 以下仅为可选示例，按需启用，避免误过滤。
# watch_uid=0
# watch_uid=501
# exclude_uid=501
# watch_gid=0
# watch_gid=501
# exclude_gid=501
# watch_ppid=1
# exclude_ppid=1234
```

`exclude_process` 匹配 `data.proc` 的完整名称，区分大小写，不接受 PID、完整程序路径、通配符或子串。无法取得进程名的事件仍可通过其他规则。短生命周期进程可能在事件消费前退出，系统也可能截断名称，应以实际 `data.proc` 为准。

UID/GID 是后端提供的事件属性。**iOS `devfsev` 的 UID/GID 表示文件所有者，不保证等于执行操作进程的 UID/GID**；例如 root 修改 mobile 拥有的文件，事件 UID 仍可能为 501。fsmod 不用自身 UID、后续 `stat()` 或推断凭据补全。当前其他后端没有可用的事件 UID/GID；JSON 中的 0 也可能表示缺失。

每个 ID 维度独立：未配置该维度时不限制；设置 `watch_*` 后至少命中一个白名单值；只设置 `exclude_*` 时允许其他已知值；同值出现在两边时排除优先。一旦配置某维度的任意规则，该维度元数据未知的事件会被丢弃。

```ini
watch_uid=0
watch_uid=501
exclude_uid=501
watch_gid=0
```

以上最终只接受 **已知 UID 为 0 且已知 GID 为 0** 的事件。UID 与 GID 名单不按行配对；维度内部白名单是 OR，UID、GID、PPID、路径、事件类型及进程过滤之间是 AND。

PPID 匹配 `data.ppid`，即事件消费时查询到的操作进程直接父 PID，不是祖先、父进程名或稳定身份。`watch_ppid=1` 只匹配直接父 PID 为 1 的进程，不包含 PID 1 的全部后代。`devfsev`/Linux `fanotify` 尝试查询 PPID，`fsevapi`/`inotify` 不提供；进程退出与 PID 重用会影响结果。配置任一 PPID 规则后，未知或非正 PPID 会被丢弃。

## 临时控制已启动的 fsmod

`ctl` 用于更新其他 deb 包等维护过程，不必停止守护进程或卸载 launchd 服务。以下以 Rootful 为例，Rootless 把命令路径替换成 `/var/jb/usr/sbin/fsmod`。

```sh
/usr/sbin/fsmod ctl status
/usr/sbin/fsmod ctl status --json

# 同时暂停事件日志与 HTTP，本例有效期 600 秒。
/usr/sbin/fsmod ctl pause --id com.example.package --scope all --ttl 600
/usr/sbin/fsmod ctl resume --id com.example.package

# 仅暂停 HTTP；log_events 仍按静态配置输出。
/usr/sbin/fsmod ctl pause --id com.example.package --scope report --ttl 600
# 完成维护后恢复：
/usr/sbin/fsmod ctl resume --id com.example.package

# 同时丢弃暂停前已排队的 HTTP 事件。
/usr/sbin/fsmod ctl pause --id com.example.package --scope all --ttl 600 --discard-pending
/usr/sbin/fsmod ctl resume --id com.example.package

# 包安装前暂停，安装结束后自动释放本次暂停。
/usr/sbin/fsmod ctl run --scope all --ttl 600 -- dpkg -i yourpackage.deb
```

| 状态 | 内核事件读取 | 事件日志 | HTTP |
| --- | --- | --- | --- |
| 正常 | 读取并处理 | 按 `log_events` | 发送。 |
| `all` | 持续读取并丢弃 | 暂停 | 暂停。 |
| `report` | 读取并处理 | 按 `log_events` | 暂停。 |

暂停不关闭内核事件流。暂停期间对应输出的事件被丢弃，恢复后不补报；默认保留暂停前的待发送队列，最后一个暂停释放后再发送。`--discard-pending` 删除旧队列，暂停状态下退出也不会发送保留队列。事件队列始终只在内存中。

暂停按 ID 独立持有，ID 为 1 至 127 个 ASCII 字母、数字、点、下划线或连字符；最多同时 64 个。默认 scope 为 `all`，默认 TTL 为 600 秒，范围 `1..86400` 秒。同 ID 再次 pause 会更新 scope 并续期，不同 ID 可以共存。resume 只释放指定 ID，未知 ID 视为成功。全部相关暂停释放/到期后才恢复，`all` 优先于 `report`。

`pause` 成功返回前会阻断后续处理，并等待正在发送的请求完成或取消。维护操作应在 pause 成功后开始；请求已到达服务端时无法撤回。命令失败/超时可能仍已建立暂停，可用 status 核实。`devfsev` 还用内核原始时间戳过滤恢复边界前的延迟事件；无内核时间戳的后端按回调时间判断，恢复后才送达的延迟事件可能被保留。

`ctl run` 的选项必须在 `--` 前，之后为直接执行的程序与参数，不进行 shell 展开。它生成独立 ID，暂停确认后才执行命令，命令退出后仅释放自己的 ID；不接受 `--id` 或 `--json`。标准输入/输出/错误传给子程序，控制诊断写 stderr，不向子程序 stdout 插入 JSON。暂停失败或服务不可达则不执行子命令。

包装命令保留子程序退出码；不存在程序为 127，其他启动失败为 126，信号退出为 `128 + signal`。正常退出、启动失败及可捕获的 `SIGINT`/`SIGTERM`/`SIGHUP` 会尝试释放暂停，信号传给子进程组。清理失败可使本来成功的子命令最终返回非零，但不覆盖已有非零退出码。支持前台输入，不支持 `Ctrl-Z`/`fg`/`bg` 作业控制。

**`run` 不自动续期 TTL。** 应设置为比预计维护时间长；到期即恢复，即使命令尚未结束。`SIGKILL` 无法触发清理，只能依赖到期恢复。脱离或晚于直接子程序退出的后代进程不在包装命令生命周期内。

本地控制使用私有 Unix socket 并校验连接 UID，应以守护进程所属用户执行。默认 socket 是 `pid_file` 加 `.ctl`，包内默认 `/var/run/fsmod.pid.ctl`；状态文件是 `pid_file` 加 `.ctl.state`。同一次开机内重启守护进程会恢复未到期暂停，重启设备则作废旧暂停。状态文件只保存暂停，不保存事件和计数器，维护期间不要删除。

使用自定义 PID 文件或配置已被修改时可显式指定 socket：

```sh
/usr/sbin/fsmod ctl status --json --socket /var/run/fsmod.pid.ctl
/usr/sbin/fsmod -C /path/to/custom.conf ctl status --json
```

socket 路径过长时派生为 `/tmp/fsmod-<uid>-<path-hash>.ctl`，状态文件仍在 PID 文件旁。`status`/`pause`/`resume` 返回 JSON，状态包括 `pid`、`monitor_paused`、`report_paused`、`queued`、`in_flight`、`delivered`、`failed`、`dropped`、`suppressed_monitor`、`suppressed_report` 和 `holds`；每个 hold 包含 `id`、`scope`、`remaining_seconds`，计数仅属于当前进程。

## HTTP 协议与字段

每个事件发送一次 JSON POST：

```http
POST /events HTTP/1.1
Content-Type: application/json
Authorization: Bearer <仅配置 report_token 时发送>
```

示例值仅用于说明：

```json
{
  "source": "fsmod",
  "data": {
    "hostname": "iPhone",
    "uuid": "example-device-udid",
    "os_name": "iOS",
    "os_version": "14.5.1",
    "kernel_version": "20.4.0",
    "backend": "devfsev",
    "observed_at": "2026-10-26T09:47:25.450Z",
    "type": "FSE_CLONE",
    "filename": "/sbin/source.txt",
    "newfile": "/sbin/cloned.txt",
    "proc": "fsmod-copy-probe",
    "event": null,
    "pid": 32075,
    "ppid": 21811,
    "uid": 0,
    "gid": 0,
    "inode": 1152921500312295620,
    "mode": 33152,
    "dev_major": 1,
    "dev_minor": 5,
    "kernel_timestamp": 114655000758862
  }
}
```

`source` 固定为 `fsmod`，识别上报程序；`data` 是 JSON 对象。`log_events=true` 也输出相同格式，交互式 `-j`/`-J` 保持原有格式。

| 字段 | 说明 |
| --- | --- |
| `hostname` | 每次序列化时读取的主机名，读取失败时为 `unknown`。 |
| `uuid` | 设备/系统安装标识。iOS 通过 MobileGestalt 读取 `UniqueDeviceID`；macOS 为 `gethostuuid()`；Linux 为 machine-id。读取失败为 `null`，不随机补值。 |
| `os_name`、`os_version` | 系统名称及产品/发行版版本；Apple 取产品版本，Linux 取 os-release。缺失为 `null`。 |
| `kernel_version` | `uname` 内核版本，Apple 为 Darwin 版本，不等于 iOS/macOS 产品版本。 |
| `backend` | 当前事件后端。 |
| `observed_at` | fsmod 序列化事件时的 UTC 时间。 |
| `type` | 统一 `FSE_*` 事件名。 |
| `filename`、`newfile` | 单路径事件的路径，以及双路径事件的第一/第二路径；rename 为旧/新路径，clone 为源/目标路径。普通复制通常 `newfile=null`。 |
| `proc`、`pid`、`ppid` | 可用的操作进程名、PID 与直接父 PID。 |
| `uid`、`gid` | 后端事件身份信息，`devfsev` 为文件所有者，详见过滤章节。 |
| `inode`、`mode`、`dev_major`、`dev_minor` | 后端提供的 inode、文件模式和设备号。 |
| `kernel_timestamp` | `devfsev` 内核事件的原始 64 位 `mach_absolute_time()` tick，不是 Unix 时间、秒或毫秒。其他后端没有该值时为 0。 |
| `event` | 后端附加事件信息，可能为 `null`。 |

元数据是否可用取决于后端，0/null 不必然代表真实值，不能单凭 `uid=0` 断定是 root 进程。inode、内核时间戳可超过浮点数精确表示范围，接收端应保留 64 位整数或原始 JSON，避免精度损失。

iOS UDID 可能为 25 或 40 字符，不一定是 RFC 4122 UUID。主机标识不用于认证，系统克隆、重装等可能影响唯一性。设备标识、系统及内核版本在每进程启动上报时采集并缓存，系统升级或身份变化后需重启刷新；`hostname` 则每次序列化时读取。MobileGestalt 权限失败会记录告警，该进程后续报告保留 `uuid=null`。

仅 HTTP **200** 视为成功，允许响应体非空，fsmod 不解析其内容。201、204、重定向和其他状态都视为失败，按配置重试。发送是单 worker 顺序执行；重试耗尽后丢弃，没有磁盘队列、崩溃恢复或历史补报。服务端已保存但响应丢失时，客户端重试可能产生重复。

### OpenResty 与 Redis

仓库接收端沿用 `redisUtil.lua` 的 `new(opts):exec(func)` 接口，配置直接位于 `events.lua` 顶部，不需要单独 `config.lua`。完整部署见 [服务端说明](server/openresty/README.md)。

处理器校验 envelope，固定 `SELECT 3`，使用 `LPUSH fsmod:events <原始JSON>` 保存完整请求体，最新数据在表头。写入成功返回：

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{"code":200,"message":"ok"}
```

检查最近十条：

```sh
redis-cli -h 127.0.0.1 -p 6379 -n 3 LRANGE fsmod:events 0 9
```

接收端默认没有 List 裁剪、TTL 或去重。Redis 持久化、备份、保留策略由部署者设置；HTTP 200 表示写命令成功返回，不表示已刷盘。Redis Cluster 不支持该 DB 3 多数据库用法。

## 桌面构建与运行

Linux 构建需要 C 编译器、make 和 libcurl 开发文件；测试还需要 Python 3：

```sh
sudo apt-get install libcurl4-openssl-dev pkg-config
make
sudo make install PREFIX=/usr
```

macOS 安装 Xcode Command Line Tools 后可使用系统 libcurl：

```sh
make CURL_CFLAGS= CURL_LIBS=-lcurl
sudo make install PREFIX=/usr/local
```

桌面上报要求支持异步 DNS 的 libcurl 7.32.0 及以上；HTTPS 还需该库支持 TLS。构建可通过 `CURL_CFLAGS`/`CURL_LIBS` 指定依赖。`pkg-config`/`curl-config` 仅在构建时用于发现依赖，不是运行时上报命令。仓库的 Homebrew 配方仍指向上游发布版，不含此工作区的守护改造，应使用本仓库源码构建。

```sh
cp fsmod.conf.example fsmod.conf
chmod 600 fsmod.conf
# 修改上报地址与路径后：
./fsmod -C ./fsmod.conf -T
./fsmod -C ./fsmod.conf -F   # 前台观察，Ctrl-C 停止

# 或选择后台运行，再用 -S 停止：
./fsmod -C ./fsmod.conf -D
./fsmod -C ./fsmod.conf -S
```

`-T` 只检查配置，不采集或发 HTTP；`-F` 强制前台，桌面 `-D` 强制后台。显式指定 `-C` 且没有 `-F`/`-D` 时，桌面使用 `daemonize`。`-S` 发送 SIGTERM，成功返回不表示队列清理已结束，需保持与启动相同的 PID 文件配置。无参数或未指定 `-C` 的 `-T`/`-S` 等命令在桌面读取默认 `/etc/fsmod.conf`。

桌面服务模板位于 `dist/launchd/com.apple.fsmod.plist` 和 `dist/systemd/com.apple.fsmod.service`，使用 `/usr/local/sbin/fsmod` 无参数启动。按模板安装程序并配置 `daemonize=false`，准备 root 私有的 `/etc/fsmod.conf`，设置已存在、可写的 PID/日志父目录。安装到其他前缀时同步调整模板。

```sh
# macOS
sudo install -m 0644 dist/launchd/com.apple.fsmod.plist /Library/LaunchDaemons/
sudo launchctl bootstrap system /Library/LaunchDaemons/com.apple.fsmod.plist

# Linux
sudo install -m 0644 dist/systemd/com.apple.fsmod.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now com.apple.fsmod.service
```

由服务管理器运行时应使用其停止命令；launchd 的 `KeepAlive` 会重新启动被直接 `-S` 停止的进程。提高关闭超时后应同步检查服务管理器退出等待时间，模板为 30 秒。

## iOS 与 Android 编译

在具备 iPhoneOS SDK、Python 3 和 ldid 的 Mac 上：

```sh
make ios-packages
# 或分开执行：
make ios
python3 dist/ios/package.py --binary fsmod-ios --variant both
```

输出至 `out/ios/`，打包脚本只使用 Python 标准库，检查 arm64 物理 iOS Mach-O 与签名 load command，但不验证设备上的签名信任或实际 entitlement。更多选项见 [iOS 部署说明](dist/ios/README.md)。

Android 默认编译选项只启用交互式监控（`FSMOD_NO_REPORT=1`）。仓库保留旧版 `make android` / `android-shell.sh` 入口：脚本依赖 `make_standalone_toolchain.py` 并固定 `--api=28`，Makefile 还把 `ANDROID_API` 直接作为编译器参数传入。使用这些入口前需适配实际 NDK 工具链，不能把它们当作已验证的现代 NDK 一步构建流程。

适配目标工具链后，守护上报相关构建变量为：

| 变量 | 用途 |
| --- | --- |
| `ANDROID_REPORT=1` | 启用配置化守护上报。 |
| `ANDROID_CURL_CFLAGS` | 目标 libcurl 的头文件等编译选项。 |
| `ANDROID_CURL_LIBS` | 目标 libcurl 及其依赖的链接选项；启用上报时必填。 |

目标 libcurl 必须按对应 ABI/API 构建，支持 HTTP/HTTPS 和异步 DNS。交叉编译不使用宿主机的 libcurl 路径；动态链接时还需部署目标依赖库。设备权限和后端能力仍决定可用功能。

## 交互式监控

传路径或交互参数可使用原有命令行模式。`-L` 查看当前构建后端，`-h` 查看完整帮助：

```sh
fsmod /tmp
fsmod -J /tmp
# 以下进程筛选示例使用 Apple devfsev；Linux 可用 fanotify。
fsmod -B devfsev /sbin
fsmod -B devfsev -P MQService /sbin
fsmod -B devfsev -p 1234 -c /sbin
fsmod -a 30 /tmp
```

`-j`/`-J` 输出 JSON，交互式 `-P` 对进程名做子串匹配，`-p` 按 PID、`-c` 包含直接子进程，`-a` 定时停止。进程过滤依赖后端，`fsevapi`/`inotify` 无法提供对应进程信息。配置项 `exclude_process` 则是精确匹配；交互模式与配置守护是不同入口。

## 常见问题

### `kernel fsevents queue dropped events`

表示内核队列已丢事件，监控继续但缺口不能补回。当前使用 4096 深度和 64 KiB 批量读取改善突发负载；持续高事件量仍可能丢失。减少监控范围和文件操作风暴，检查 CPU/I/O 压力。`exclude_path`、`watch_events` 等在回调阶段过滤，不能直接减少内核订阅量；增加 HTTP `queue_capacity` 也不能修复内核队列缺口。

### `HTTP delivery failed ... best effort; no persistent queue`

表示 HTTP 传输失败或状态不是 200，最终重试未成功。检查设备到 `report_url` 的连通性、OpenResty 监听和路径、服务端错误日志、token，以及 Redis 连接/认证/DB 3 写入。启动出现 `fsmod started` 仅说明采集初始化成功，不证明接口可达。

### 创建文件却没有上报

先开启 `log_events=true` 并重启，确认当前配置与进程版本，核对规范化后的路径是否被纳入、是否被更具体规则排除、`watch_events` 是否允许、进程/ID 过滤和 `ctl status` 是否暂停。再区分本地没事件还是网络上报失败。日志/PID 文件与守护进程自己的事件会被抑制，前台日志若写入监控目录还应排除目标文件以避免反馈。

### `Service is disabled` / 找不到服务

禁用状态先执行 `launchctl enable system/com.apple.fsmod`，然后 bootstrap 对应布局的 plist。`Could not find service` 表示该 domain 没有加载服务，不能仅用 kickstart；确认 plist 路径、Label、权限与安装结果。不要把 Rootless 安装路径与 Rootful 混用。

### 为什么复制没有源文件

普通 libc 字节复制只触发目标变化事件，源文件读取没有明确复制关系。可用 `make ios-copy-probe` 生成诊断程序，分别测试 `clonefile()` 与字节复制；详见 iOS 说明。未出现 `FSE_CLONE` 不等同于监控器漏掉了一个原本存在的复制事件。

## 测试

在配置好开发依赖的 macOS/Linux 上执行：

```sh
make test

# 以下 Apple 专项目标在 macOS 上执行：
make test-devfsev test-cfnetwork test-ios-metadata test-ios-package
```

常规测试覆盖配置、路径规则、UID/GID/PPID、上报、队列、控制与进程生命周期，并通过本地 HTTP 服务验证前后台流程。Apple 专项目标需对应框架。OpenResty 环境下可执行 `resty tests/test_openresty.lua`，该测试模拟 Redis，不连接真实库。自动化测试不能代替越狱目标设备上的兼容性验证。

## 许可证

本项目沿用 MIT 许可证，原项目作者为 Sergi Alvarez（pancake / NowSecure）。参见 [LICENSE.md](LICENSE.md)。
