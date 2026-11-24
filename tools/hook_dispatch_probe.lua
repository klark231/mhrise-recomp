-- Live contract against the game's own System.Math and System.Array methods.
local actions = sdk.PreHookResult
assert(actions.CALL_ORIGINAL == 0 and actions.SKIP_ORIGINAL == 1)
local scope = {scope = "reflection"}
local maximum = assert(sdk.find_type_definition("System.Math"):get_method("Max(System.Int32,System.Int32)"))
local trace, mode, depth = {}, "original", 0
local function record(label) trace[#trace+1] = label .. depth end
sdk.hook(maximum, function(args)
    if mode == "idle" then return end
    record("A")
    local frame = assert(thread.get_hook_storage())
    frame.depth = depth
    assert(args[1] == sdk.get_thread_context())
    if mode == "nested" and depth == 0 then
        depth = 1
        assert(maximum:call(nil, 4, 8) == 8)
        depth = 0
        assert(thread.get_hook_storage() == frame)
    end
    if mode == "modify" then
        args[2] = 19
        return args
    end
    if mode == "skip" then return actions.SKIP_ORIGINAL end
end, function(result)
    if mode == "idle" then return result end
    record("a")
    assert(thread.get_hook_storage().depth == depth)
    if mode == "skip" then return result + 1 end
    return result
end, scope)
sdk.hook(maximum, function(args)
    if mode == "idle" then return end
    record("B")
    assert(not next(thread.get_hook_storage()))
    if mode == "modify" then assert(args[2] == 19) end
    return actions.CALL_ORIGINAL
end, function(result)
    if mode == "idle" then return result end
    record("b")
    if mode == "skip" then assert(result == nil); return 40 end
    return result
end, scope)
assert(maximum:call(nil, 2, 7) == 7 and table.concat(trace) == "A0B0b0a0")
mode,trace="modify",{}
assert(maximum:call(nil, 2, 7) == 19)
mode,trace="skip",{}
assert(maximum:call(nil, 2, 7) == 41 and table.concat(trace) == "A0B0b0a0")
mode,trace="nested",{}
assert(maximum:call(nil, 2, 7) == 7 and table.concat(trace) == "A0A1B1b1a1B0b0a0")
assert(thread.get_hook_storage() == nil)
mode="idle"

local array = sdk.create_managed_array("System.Int32", 2)
local setter = assert(sdk.find_type_definition("System.Array"):get_method("SetValue(System.Object,System.Int32)"))
array[0] = 13
local calls = 0
sdk.hook(setter, function(args)
    if args[2] == array and args[4] == 0 then
        thread.get_hook_storage().skipped=true
        return actions.SKIP_ORIGINAL
    end
end, function()
    if thread.get_hook_storage().skipped then calls=calls+1 end
end, scope)
array[0] = 27
array[1] = 31
assert(calls == 1 and array[0] == 13 and array[1] == 31)
log.info("hook dispatch passed: reflection CALL/SKIP, nested storage, ABba, skipped setter kept 13, ordinary setter wrote 31")

-- An invalid edit in one pre-hook is discarded before the next subscriber.
local absolute = assert(sdk.find_type_definition("System.Math"):get_method("Abs(System.Int32)"))
local check_fault, bad_calls, good_calls = true, 0, 0
sdk.hook(absolute,function(args)
    if check_fault then bad_calls=bad_calls+1; args[2]="invalid Int32" end
end,nil,scope)
sdk.hook(absolute,function(args)
    if check_fault then assert(args[2]==-6); good_calls=good_calls+1 end
end,nil,scope)
assert(absolute:call(nil,-6)==6 and absolute:call(nil,-6)==6)
assert(bad_calls==1 and good_calls==2 and thread.get_hook_storage()==nil)
check_fault=false
log.info("hook dispatch passed: bad subscriber isolated, original argument and other subscriber preserved")

-- Independently observe a game-originated call without invoking it from Lua.
local loading = assert(sdk.find_type_definition("snow.NowLoading"):get_method("update"))
local pending, done = {}, false
sdk.hook(loading, function()
    if done then return end
    local id=thread.get_id()
    pending[id]=(pending[id] or "").."A"
end, function()
    if done then return end
    local id=thread.get_id()
    assert(pending[id]=="ABb")
    done=true
    log.info("hook dispatch passed: game-originated NowLoading.update ABba")
end)
sdk.hook(loading, function()
    if not done then local id=thread.get_id(); pending[id]=pending[id].."B" end
end, function()
    if not done then local id=thread.get_id(); pending[id]=pending[id].."b" end
end)
