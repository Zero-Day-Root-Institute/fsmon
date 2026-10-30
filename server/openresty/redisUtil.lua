-- Compatible with the application's existing redisUtil.new(opts):exec(func) API.
local redis = require "resty/redis"

local _M = {}
local mt = { __index = _M }

local function failed(red, operation, err)
    -- Never write errors to the response: callers control their HTTP status/body.
    ngx.log(ngx.ERR, "Redis: ", operation, " failed")
    if red then red:close() end
    return nil, err or (operation .. " failed")
end

function _M.exec(self, func)
    local red, err = redis:new()
    if not red then return failed(nil, "initialize", err) end
    red:set_timeout(self.timeout)

    local ok
    ok, err = red:connect(self.host, self.port)
    if not ok then return failed(red, "connect", err) end

    if self.password ~= "" then
        local count
        count, err = red:get_reused_times()
        if count == nil then return failed(red, "get_reused_times", err) end
        if count == 0 then
            ok, err = red:auth(self.password)
            if not ok then return failed(red, "AUTH", err) end
        end
    end

    -- Pooled connections retain their selected database; select on every checkout.
    ok, err = red:select(self.database)
    if not ok then return failed(red, "SELECT", err) end

    local res
    res, err = func(red)
    if res == nil or res == false then return failed(red, "command", err) end

    ok = red:set_keepalive(self.max_idle_time, self.pool_size)
    if not ok then red:close() end
    -- The command already succeeded even if returning its socket to the pool fails.
    return res, err
end

function _M.new(opts)
    local config = opts or {}
    return setmetatable({
        host = config.host or "127.0.0.1",
        password = config.password or "",
        port = config.port or 6379,
        timeout = config.timeout or 5000,
        database = config.database or 0,
        max_idle_time = config.max_idle_time or 600000,
        pool_size = config.pool_size or 1000,
    }, mt)
end

return _M
