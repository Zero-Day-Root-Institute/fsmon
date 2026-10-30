# OpenResty 接收事件并写入 Redis DB 3

本目录提供 `POST /events` 接收端示例，接收 fsmod 的
`{"source":"fsmod","data":{...}}`，将完整原始 JSON 请求体写入 Redis
数据库 **3** 的 List `fsmod:events`。这里只提供部署文件和操作说明，**没有在你的服务器上安装、启动或部署服务**。

## 行为约定

- 仅接收 POST，Content-Type 为 `application/json`，支持附加 `charset=utf-8`。
- 顶层必须是 JSON 对象，`source` 必须为固定字符串 `fsmod`，`data` 必须是对象；接收端校验基本必填事件字段。
- 使用 `LPUSH fsmod:events <原始请求体>`，最新事件位于 List 头部。请求体不会经过 JSON 解码后重编码，避免将 `kernel_timestamp` 等 64 位整数转换后损失精度。
- 数据库固定为 **DB 3**，包括从连接池取回连接后也重新执行 `SELECT 3`；不提供修改数据库编号的配置。Redis Cluster 不支持此多数据库用法。
- 仅 Redis 写入成功后返回 `200 OK`，Content-Type 为 `application/json; charset=utf-8`，响应体为 `{"code":200,"message":"ok"}`。接收端不自行重试 Redis 写入。
- 默认监听 `127.0.0.1:8080`，未配置 bearer token 时只适合本机接入。跨主机部署应配置 HTTPS 和 bearer token，并限制访问范围。
- 默认请求体上限 1 MiB；Redis 连接、发送和读取超时均为 1000 ms；空闲连接保存 10000 ms，连接池容量为 100。
- 默认没有 TTL 和 List 裁剪，事件会持续累积。Redis 的 AOF/RDB、备份、容量与保留策略由服务端管理员配置，HTTP 200 不等同于已经刷盘。

## 部署

需要已经安装并运行的 OpenResty 与 Redis。Lua 使用 OpenResty 环境中的
`cjson.safe`、`resty.redis` 和仓库内的 `redisUtil.lua`。以下命令在仓库根目录执行，假设 OpenResty
worker 的系统组为 `www-data`；请根据实际 `nginx.conf` 中的 `user` 改为
对应组，确保 worker 可以读取 Lua 文件。

```sh
sudo install -d -m 0750 -o root -g www-data /etc/fsmod/openresty
sudo install -m 0640 -o root -g www-data server/openresty/redisUtil.lua /etc/fsmod/openresty/redisUtil.lua
sudo install -m 0640 -o root -g www-data server/openresty/events.lua /etc/fsmod/openresty/events.lua
```

编辑 `/etc/fsmod/openresty/events.lua` 顶部的 `local config` 表。默认 Redis 是
`127.0.0.1:6379`，写入键为 `fsmod:events`。如果 Redis 启用了密码认证，
按实际服务设置 `redis_password`；不使用 Redis 认证时保留默认空值。可设置 `bearer_token`，
此时客户端 `report_token` 必须与它一致。保护包含口令的 Lua 文件，避免将
实际凭据提交到仓库。修改配置后需要重新加载 OpenResty。

所有配置直接写在 `events.lua` 顶部，按需修改：

```lua
local config = {
    redis_host = "127.0.0.1",
    redis_port = 6379,
    redis_password = "", -- redisUtil 使用密码认证；空字符串表示不认证
    redis_key = "fsmod:events",
    bearer_token = nil,   -- 跨主机接入时设置长随机 token
    max_body_bytes = 1048576,
    redis_timeout_ms = 1000,
    redis_keepalive_ms = 10000,
    redis_pool_size = 100,
}
```

`redis_password` 对应 Redis 的密码认证，与 HTTP 的 `bearer_token` 无关。
`redisUtil.lua` 按你现有的 `new(opts):exec(func)` 接口工作：新连接认证，
复用连接跳过重复认证；每次从连接池取出连接后仍执行 `SELECT 3`。数据库编号
没有配置字段，处理器始终使用 3。
如果调整 `redis_key`，下面的 Redis 查询命令也要使用相同键名。
调整请求体大小时，同时修改 Lua 的 `max_body_bytes` 和 Nginx 示例中的
`client_max_body_size`；任一侧的限制都可能先返回 413。

`nginx.conf.example` 是 HTTP 配置片段：它包含 `lua_package_path` 和
`server`，应在现有 Nginx/OpenResty 配置的 **`http { ... }` 内** include。
不要把整个片段放进现有 `server` 或 `location`，也不要直接 include 到
`http` 外的主配置层。示例通过以下设置加载处理脚本：

```nginx
lua_package_path '/etc/fsmod/openresty/?.lua;;';

server {
    listen 127.0.0.1:8080;
    location = /events {
        content_by_lua_file /etc/fsmod/openresty/events.lua;
    }
}
```

安装并 include 仓库中的完整示例文件；上面的代码仅解释作用域，不代替完整文件的请求体上限等设置。
已有站点使用 8080 端口时先调整监听配置，避免重复监听或路径冲突。

```sh
sudo install -m 0644 server/openresty/nginx.conf.example /etc/fsmod/openresty/nginx.conf
```

在现有配置中增加：

```nginx
http {
    # 保留已有的其他配置。
    include /etc/fsmod/openresty/nginx.conf;
}
```

**不要再创建第二个 `http` 块**；将 include 行加入已有块即可。若已有
`lua_package_path`，合并搜索路径，保留末尾 `;;`，确保能加载 `redisUtil.lua`。

检查后重新加载已运行的实例：

```sh
sudo openresty -t
sudo openresty -s reload
```

若 OpenResty 使用非默认 `-p` / `-c` 路径，对检查和 reload 使用与实际服务
相同的参数。尚未启动的实例应按所在系统的服务管理方式启动；reload 不负责首次启动。

## fsmod 客户端

在客户端配置中指定 endpoint 和实际监控路径，例如：

```ini
report_url=http://127.0.0.1:8080/events
# 服务端启用 bearer_token 时填写完全相同的值：
# report_token=replace-with-your-bearer-token
watch_path=/tmp
daemonize=false
pid_file=/tmp/fsmod.pid
log_file=/tmp/fsmod.log
```

将配置保存为受保护的 `/etc/fsmod.conf` 后验证并前台运行：

```sh
fsmod -C /etc/fsmod.conf -T
fsmod -C /etc/fsmod.conf -F
```

`127.0.0.1` 指客户端本机。跨机器时替换为配置好的服务端 HTTPS 地址，
并同时调整 OpenResty 监听、证书和访问控制。协议中的 `source="fsmod"`
标识上报程序，`data.uuid` 标识设备/系统安装；其他字段也都在 `data` 中，
例如 `data.filename`。配置 `log_events=true` 的本地事件日志使用相同 envelope。

## 手动联调

以下数据都是说明性样例，不代表当前机器的 UUID、系统或实际文件事件。
保存为一个 JSON 对象，`data` 不要再编码成字符串：

```sh
cat > /tmp/fsmod-event.json <<'JSON'
{
  "source": "fsmod",
  "data": {
    "hostname": "example-host",
    "uuid": "0123456789abcdef0123456789abcdef",
    "os_name": "Example Linux",
    "os_version": "1.0",
    "kernel_version": "6.0.0-example",
    "backend": "inotify",
    "observed_at": "2026-10-26T08:00:00.123Z",
    "type": "FSE_CONTENT_MODIFIED",
    "filename": "/tmp/example.txt",
    "newfile": null,
    "proc": null,
    "event": null,
    "pid": 0,
    "ppid": 0,
    "uid": 0,
    "gid": 0,
    "inode": 0,
    "mode": 0,
    "dev_major": 0,
    "dev_minor": 0,
    "kernel_timestamp": 18446744073709551615
  }
}
JSON

curl --silent --show-error --fail \
  --request POST \
  --header 'Content-Type: application/json; charset=utf-8' \
  --data-binary @/tmp/fsmod-event.json \
  --dump-header - \
  http://127.0.0.1:8080/events
```

成功响应示例：

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{"code":200,"message":"ok"}
```

启用 bearer token 后，此手动请求也需要
携带 `Authorization: Bearer <token>`。可将该 header 放入仅本人可读的
curl config 文件，用 `curl --config /path/to/private-curl.conf ...` 加载，
避免把实际 token 写入命令行。服务端返回错误时先检查 OpenResty error log
以及 Redis 的连接、认证和权限配置。

检查 **DB 3** 中的数据：

```sh
redis-cli -h 127.0.0.1 -p 6379 -n 3 TYPE fsmod:events
redis-cli -h 127.0.0.1 -p 6379 -n 3 LLEN fsmod:events
redis-cli -h 127.0.0.1 -p 6379 -n 3 LRANGE fsmod:events 0 9
```

`TYPE` 应为 `list`；`LRANGE ... 0 9` 取最近十条完整原始 JSON。认证参数
应与部署的 Redis 一致；不要遗漏 `-n 3` 而误查默认 DB 0。同一请求的
重试可造成重复记录，不要将 List 长度直接当作去重事件数。

## 接口测试

在安装了 OpenResty 的机器上，从仓库根目录运行：

```sh
resty tests/test_openresty.lua
```

测试使用真实 JSON 解码器，并模拟 HTTP 上下文和 Redis，覆盖请求校验、
DB 3 选择、原始 JSON 保存、认证及异常路径；不会连接真实 Redis。
部署后再使用前面的 curl 和 redis-cli 命令完成实际联调。

## 错误与交付边界

| HTTP 状态 | 含义 |
| --- | --- |
| `200` | Redis 的 LPUSH 已成功返回；响应体为 `{"code":200,"message":"ok"}`。 |
| `400` | JSON、envelope 或必填事件字段不符合要求。 |
| `401` | 已启用 bearer token，但请求缺少或使用了错误凭据。 |
| `405` | 请求方法不是 POST。 |
| `413` | 请求体超过限制，默认 1 MiB。 |
| `415` | Content-Type 不是 application/json。 |
| `500` | 无法读取 Nginx 暂存的请求体，或服务端存储配置不完整。 |
| `503` | Redis 客户端初始化、连接、认证、SELECT 3 或写入失败。 |

客户端仅将 HTTP `200` 视为成功，忽略响应体，因此支持上述非空 JSON 响应；
其他状态（包括 `201` 和 `204`）均按其配置重试。
若 Redis 已收到写入但响应在途中丢失，
重试仍可能产生重复。接收端不额外重试、不默认去重，也不提供持久队列或
事件消费接口。重启、持久化、备份、数据保留和下游去重需要按部署需求配置。
