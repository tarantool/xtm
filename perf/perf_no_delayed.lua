local log = require ('log')
local fiber = require('fiber')
local libppq = require ('libppq')

local stat = {}
rawset(_G, 'stat', stat)

local chan = fiber.channel()
local ppq = libppq.new {
    on_ctx = function(self, msg)
        if msg.id < 1e6 then
            stat[msg.id] = msg
            self:send(msg.id+1)
        else
            chan:put(true)
        end
    end,
}
rawset(_G, 'ppq', ppq)

fiber.create(function()
    ppq:run()
end)

local time_before = fiber.time()
fiber.create(function()
    ppq:send(0)
end)
chan:get()
local time_after = fiber.time()

local n = #stat
local floor = math.floor

log.info("message count per second: %d", 1e6 / (time_after - time_before))
log.info("----------------")

table.sort(stat, function(a, b) return a.rtt < b.rtt end)
for _, p in ipairs{0.5, 0.9, 0.99, 0.999, 0.9999} do
	log.info("%.2f%% rtt: %.4fµs", p*100, stat[floor(n*p)].rtt)
end

log.info("----------------")

table.sort(stat, function(a, b) return a.syn < b.syn end)
for _, p in ipairs{0.5, 0.9, 0.99, 0.999, 0.9999} do
	log.info("%.2f%% syn: %.4fµs", p*100, stat[floor(n*p)].syn)
end

log.info("----------------")

table.sort(stat, function(a, b) return a.ack < b.ack end)
for _, p in ipairs{0.5, 0.9, 0.99, 0.999, 0.9999} do
	log.info("%.2f%% ack: %.4fµs", p*100, stat[floor(n*p)].ack)
end

os.exit(0)
