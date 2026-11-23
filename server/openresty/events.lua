-- POST /events: store the original {source="fsmod",data={...}} in Redis DB 3.
local json = require "cjson.safe"
local redisUtil = require "redisUtil"

-- Receiver settings. Edit here and reload OpenResty after making changes.
local config = {
    redis_host = "127.0.0.1",
    redis_port = 6379,
    redis_password = "", -- Empty disables Redis password authentication.
    redis_key = "fsmod:events", -- Redis List, newest report first.
    bearer_token = nil, -- Match the client report_token when enabled.
    max_body_bytes = 1024 * 1024,
    redis_timeout_ms = 1000,
    redis_keepalive_ms = 10000,
    redis_pool_size = 100,
}

local function fail(status, message)
    ngx.status = status
    ngx.header["Content-Type"] = "application/json; charset=utf-8"
    ngx.say(json.encode({ error = message }))
    return ngx.exit(status)
end

local function server_error(operation)
    -- Do not log request bodies, bearer tokens, or Redis credentials.
    ngx.log(ngx.ERR, "fsmod receiver: Redis ", operation, " failed")
    return fail(503, "event storage unavailable")
end

if ngx.req.get_method() ~= "POST" then
    ngx.header["Allow"] = "POST"
    return fail(405, "POST required")
end

local content_type = ngx.var.content_type or ""
local media_type = content_type:match("^%s*([^;]+)") or ""
media_type = media_type:gsub("%s+$", ""):lower()
if media_type ~= "application/json" then
    return fail(415, "Content-Type must be application/json")
end

if config.bearer_token and config.bearer_token ~= "" then
    if ngx.var.http_authorization ~= "Bearer " .. config.bearer_token then
        ngx.header["WWW-Authenticate"] = 'Bearer realm="fsmod"'
        return fail(401, "invalid bearer token")
    end
end

local limit = config.max_body_bytes or 1048576
local declared_length = tonumber(ngx.var.content_length)
if declared_length and declared_length > limit then
    return fail(413, "request body too large")
end

ngx.req.read_body()
local body = ngx.req.get_body_data()
if not body then
    -- Nginx may spool larger request bodies to a temporary file.
    local path = ngx.req.get_body_file()
    if path then
        local file = io.open(path, "rb")
        if not file then return fail(500, "cannot read request body") end
        body = file:read(limit + 1)
        file:close()
        if not body then return fail(500, "cannot read request body") end
    end
end
if not body or #body == 0 then return fail(400, "request body is empty") end
if #body > limit then return fail(413, "request body too large") end

-- Reject non-standard JSON values such as NaN/Infinity. The original body is
-- stored unchanged, so uint64 inode/timestamps are not rounded by re-encoding.
local decoder = json.new()
decoder.decode_invalid_numbers(false)
decoder.decode_max_depth(16)
local envelope = decoder.decode(body)
if type(envelope) ~= "table" or envelope.source ~= "fsmod" or type(envelope.data) ~= "table" then
    return fail(400, "expected source=fsmod and a data object")
end
for key in pairs(envelope) do
    if key ~= "source" and key ~= "data" then
        return fail(400, "only source and data are allowed at the root")
    end
end
local data = envelope.data
for key in pairs(data) do
    if type(key) ~= "string" then return fail(400, "data must be an object") end
end
for _, key in ipairs({ "hostname", "backend", "observed_at", "type" }) do
    if type(data[key]) ~= "string" or data[key] == "" then
        return fail(400, "missing or invalid data." .. key)
    end
end
-- Some backends can report an event without a path; the wire format permits null.
if data.filename ~= json.null and (type(data.filename) ~= "string" or data.filename == "") then
    return fail(400, "missing or invalid data.filename")
end
for _, key in ipairs({ "uuid", "os_name", "os_version", "kernel_version", "newfile", "proc", "event" }) do
    local value = data[key]
    if value ~= nil and value ~= json.null and type(value) ~= "string" then
        return fail(400, "invalid data." .. key)
    end
end
for _, key in ipairs({ "pid", "ppid", "uid", "gid", "inode", "mode", "dev_major", "dev_minor", "kernel_timestamp" }) do
    local value = data[key]
    if value ~= nil and (type(value) ~= "number" or value % 1 ~= 0) then
        return fail(400, "invalid data." .. key)
    end
end

-- Use the application's existing redisUtil.new/exec connection lifecycle.
-- The receiver always selects database 3 through the utility's database option.
local store = redisUtil.new({
    host = config.redis_host or "127.0.0.1",
    port = config.redis_port or 6379,
    password = config.redis_password or "",
    timeout = config.redis_timeout_ms or 1000,
    database = 3,
    max_idle_time = config.redis_keepalive_ms or 10000,
    pool_size = config.redis_pool_size or 100,
})
local length = store:exec(function(red)
    return red:lpush(config.redis_key or "fsmod:events", body)
end)
if not length then return server_error("write") end
ngx.status = 200
ngx.header["Content-Type"] = "application/json; charset=utf-8"
ngx.say(json.encode({ code = 200, message = "ok" }))
return ngx.exit(200)
