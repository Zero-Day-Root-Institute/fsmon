-- Run from the repository root: resty tests/test_openresty.lua
-- Or: luajit tests/test_openresty.lua (with cjson.safe). No real Redis required.
local json = require "cjson.safe" -- Use the real decoder, including null/uint64.
local handler = "server/openresty/events.lua"
local handler_file = assert(io.open(handler, "rb"))
local handler_source = assert(handler_file:read("*a"))
handler_file:close()
-- Override inline settings only in the test chunk; no production test hooks.
local replacements
handler_source, replacements = handler_source:gsub("(local config = %b{})",
    "%1\nfor key, value in pairs(__test_settings) do config[key] = value end", 1)
assert(replacements == 1, "expected one inline receiver settings table")
package.path = "server/openresty/?.lua;" .. package.path
package.loaded["redisUtil"] = nil
local state, current, checks = { list = {}, db = 9 }, nil, 0
local function eq(actual, expected, label)
    assert(actual == expected, (label or "value") .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual))
    checks = checks + 1
end
local function record(name, ...)
    current.calls[#current.calls + 1] = { name, ... }
end
local red = {}
function red:set_timeout(ms) record("timeout", ms) end
function red:connect(host, port, options)
    record("connect", host, port, options)
    if current.fail == "connect" then return nil, "offline" end
    return true
end
function red:get_reused_times()
    record("get_reused_times")
    if current.fail == "get_reused_times" then return nil, "socket state unavailable" end
    return current.reused_times or 0, nil
end
function red:auth(...)
    record("auth", ...)
    if current.fail == "auth" then return nil, "denied" end
    return true
end
function red:select(db)
    record("select", db)
    if current.fail == "select" then return nil, "denied" end
    state.db = db
    return true
end
function red:lpush(key, body)
    record("lpush", key, body)
    eq(state.db, 3, "selected database before write")
    if current.fail == "lpush" then return nil, "write failed" end
    table.insert(state.list, 1, body)
    current.writes = current.writes + 1
    return #state.list
end
function red:set_keepalive(...)
    record("keepalive", ...)
    if current.fail == "keepalive" then return nil, "pool full" end
    return true
end
function red:close() record("close") end
local resty_redis_mock = { new = function()
    record("new")
    if current.fail == "new" then return nil, "allocation failed" end
    return red -- Reused object deliberately retains selected DB across requests.
end }
-- redisUtil.lua follows the application's existing require "resty/redis" spelling.
package.loaded["resty.redis"] = resty_redis_mock
package.loaded["resty/redis"] = resty_redis_mock
local function names(ctx)
    local result = {}
    for _, call in ipairs(ctx.calls) do result[#result + 1] = call[1] end
    return table.concat(result, ",")
end
local function find(ctx, name)
    local found, count = nil, 0
    for _, call in ipairs(ctx.calls) do
        if call[1] == name then found, count = call, count + 1 end
    end
    return found, count
end
local function payload(change)
    local data = { hostname = "test-host", backend = "inotify", observed_at = "2026-10-26T00:00:00Z", type = "create", filename = "/tmp/a", uid = 0 }
    if change then change(data) end
    return assert(json.encode({ source = "fsmod", data = data }))
end
local valid = payload()
local function request(options)
    local opts = options or {}
    local ctx = { calls = {}, writes = 0, fail = opts.fail, output = {}, logs = {} }
    ctx.reused_times = opts.reused_times
    current = ctx
    local config = { redis_host = "127.0.0.1", redis_port = 6379, redis_key = "test:events", max_body_bytes = 4096 }
    for key, value in pairs(opts.config or {}) do config[key] = value end
    _G.ngx = { ERR = "error", header = {}, var = { content_type = opts.content_type or "application/json", content_length = opts.length, http_authorization = opts.authorization } }
    ngx.req = {
        get_method = function() return opts.method or "POST" end,
        read_body = function() ctx.read_body = true end,
        get_body_data = function() if not opts.spool then return opts.body or valid end end,
        get_body_file = function() return opts.spool and "mock-body-file" end,
    }
    ngx.say = function(message) ctx.output[#ctx.output + 1] = message end
    ngx.log = function(...) ctx.logs[#ctx.logs + 1] = { ... } end
    ngx.exit = function(status)
        if status == 200 then
            eq(ctx.writes, 1, "200 follows one confirmed write")
            eq(ngx.status, 200, "explicit success status")
            eq(ngx.header["Content-Type"], "application/json; charset=utf-8")
            eq(#ctx.output, 1, "one nonempty JSON acknowledgement")
            local acknowledgement = assert(json.decode(ctx.output[1]))
            eq(acknowledgement.code, 200)
            eq(acknowledgement.message, "ok")
        end
        ctx.status, ctx.header = status, ngx.header
        return status -- Each handler branch must return this value immediately.
    end
    local original_open = io.open
    if opts.spool then
        io.open = function(path, mode)
            eq(path, "mock-body-file"); eq(mode, "rb")
            if opts.spool_error == "open" then return nil, "missing" end
            return { read = function(_, count)
                eq(count, config.max_body_bytes + 1, "bounded spool read")
                if opts.spool_error == "read" then return nil, "unreadable" end
                return (opts.body or valid):sub(1, count)
            end, close = function() ctx.file_closed = true end }
        end
    end
    local chunk = assert(loadstring(handler_source, "@" .. handler))
    setfenv(chunk, setmetatable({ __test_settings = config }, { __index = _G }))
    local ok, result = pcall(chunk)
    io.open = original_open
    assert(ok, result)
    eq(result, ctx.status, "handler return")
    return ctx
end
local function rejected(status, opts)
    local before = #state.list
    local ctx = request(opts)
    eq(ctx.status, status); eq(#ctx.calls, 0, "no Redis during validation")
    eq(#state.list, before, "no validation writes")
    assert(type(assert(json.decode(ctx.output[1])).error) == "string")
    return ctx
end

-- Preserve whitespace, UTF-8 and uint64 bytes instead of re-encoding doubles.
local raw = '{ "source": "fsmod", "data": {"hostname":"主机","backend":"inotify","observed_at":"now","type":"create","filename":"/tmp/文档","inode":18446744073709551615,"kernel_timestamp":18446744073709551615,"uuid":null} }\n'
local first = request({ body = raw, content_type = "Application/JSON; charset=utf-8" })
eq(first.status, 200); eq(#first.output, 1)
eq(names(first), "new,timeout,connect,select,lpush,keepalive")
eq(find(first, "select")[2], 3)
eq(find(first, "lpush")[2], "test:events"); eq(find(first, "lpush")[3], raw)
eq(state.list[1], raw)
state.db = 11 -- Simulate a pooled checkout retaining another database.
local second = request({ body = valid })
eq(second.status, 200); eq(find(second, "select")[2], 3)
local _, select_count = find(second, "select"); eq(select_count, 1)
eq(state.list[1], valid); eq(state.list[2], raw, "newest report first")
local connected = find(second, "connect")
eq(connected[2], "127.0.0.1"); eq(connected[3], 6379)
local password_auth = request({ config = { redis_password = "secret" } })
eq(names(password_auth), "new,timeout,connect,get_reused_times,auth,select,lpush,keepalive")
eq(find(password_auth, "auth")[2], "secret")
local password = request({ config = { redis_password = "secret" } })
eq(#find(password, "auth"), 2); eq(find(password, "auth")[2], "secret")
local reused = request({ config = { redis_password = "secret" }, reused_times = 1 })
eq(names(reused), "new,timeout,connect,get_reused_times,select,lpush,keepalive")
local pooled = request({ fail = "keepalive" })
eq(pooled.status, 200); eq(pooled.writes, 1)
eq(names(pooled), "new,timeout,connect,select,lpush,keepalive,close")
for _, failure in ipairs({ "new", "connect", "get_reused_times", "auth", "select", "lpush" }) do
    local before = #state.list
    local ctx = request({ fail = failure, config = { redis_password = "secret" } })
    eq(ctx.status, 503); eq(ctx.writes, 0); eq(#state.list, before)
    local expected = {
        new = "new",
        connect = "new,timeout,connect,close",
        get_reused_times = "new,timeout,connect,get_reused_times,close",
        auth = "new,timeout,connect,get_reused_times,auth,close",
        select = "new,timeout,connect,get_reused_times,auth,select,close",
        lpush = "new,timeout,connect,get_reused_times,auth,select,lpush,close",
    }
    eq(names(ctx), expected[failure], "stop without retry after " .. failure)
end

local method = rejected(405, { method = "GET" }); eq(method.header.Allow, "POST")
rejected(415, { content_type = "text/plain" })
rejected(415, { content_type = "application/jsonp" })
local unauthorized = rejected(401, { config = { bearer_token = "token" } })
eq(unauthorized.header["WWW-Authenticate"], 'Bearer realm="fsmod"')
rejected(401, { config = { bearer_token = "token" }, authorization = "Bearer wrong" })
eq(request({ config = { bearer_token = "token" }, authorization = "Bearer token" }).status, 200)
for _, body in ipairs({ "", "{", "null", "true", "7", "[]", '{"source":"wrong","data":{}}', '{"source":"fsmod"}', '{"source":"fsmod","data":null}', '{"source":"fsmod","data":[]}', '{"source":"fsmod","data":[1]}', '{"source":"fsmod","data":"x"}', '{"source":"fsmod","data":1}' }) do
    rejected(400, { body = body })
end
local extra = assert(json.decode(valid)); extra.extra = true
rejected(400, { body = assert(json.encode(extra)) })
for _, field in ipairs({ "hostname", "backend", "observed_at", "type", "filename" }) do
    rejected(400, { body = payload(function(data) data[field] = nil end) })
    for _, value in ipairs({ false, "", 1 }) do
        rejected(400, { body = payload(function(data) data[field] = value end) })
    end
end
eq(request({ body = payload(function(data) data.filename = json.null end) }).status, 200)
for _, field in ipairs({ "uuid", "os_name", "os_version", "kernel_version", "newfile", "proc", "event" }) do
    eq(request({ body = payload(function(data) data[field] = json.null end) }).status, 200)
    rejected(400, { body = payload(function(data) data[field] = 1 end) })
end
for _, field in ipairs({ "pid", "ppid", "uid", "gid", "inode", "mode", "dev_major", "dev_minor", "kernel_timestamp" }) do
    rejected(400, { body = payload(function(data) data[field] = 1.5 end) })
    rejected(400, { body = payload(function(data) data[field] = "1" end) })
end
for _, number in ipairs({ "NaN", "Infinity", "-Infinity" }) do
    local invalid, replacements = valid:gsub('"uid":0', '"uid":' .. number)
    eq(replacements, 1); rejected(400, { body = invalid })
end
local too_large = rejected(413, { length = "4097" }); eq(too_large.read_body, nil)
rejected(413, { body = string.rep(" ", 4097) })
eq(request({ body = valid, config = { max_body_bytes = #valid } }).status, 200)
local spooled = request({ spool = true, body = raw })
eq(spooled.status, 200); eq(spooled.file_closed, true); eq(find(spooled, "lpush")[3], raw)
local bounded = rejected(413, { spool = true, body = string.rep(" ", 4097) }); eq(bounded.file_closed, true)
rejected(500, { spool = true, spool_error = "open" })
local unreadable = rejected(500, { spool = true, spool_error = "read" }); eq(unreadable.file_closed, true)
print("OpenResty mocked receiver tests passed (" .. checks .. " assertions)")
