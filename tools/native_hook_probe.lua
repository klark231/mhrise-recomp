-- Uses the same three-argument API as PC REFramework.
local CALL, SKIP = sdk.PreHookResult.CALL_ORIGINAL, sdk.PreHookResult.SKIP_ORIGINAL
assert(CALL==0 and SKIP==1)
local math_type=sdk.find_type_definition("System.Math")
local max=assert(math_type:get_method("Max(System.Int32,System.Int32)"))
local mode,depth,trace="idle",0,{}
local function mark(x) trace[#trace+1]=x..depth end
sdk.hook(max,function(args)
    if mode=="idle" then return end
    mark("A")
    local storage=assert(thread.get_hook_storage())
    assert(not next(storage))
    storage.depth=depth
    assert(args[1]==sdk.get_thread_context())
    if mode=="nested" and depth==0 then
        depth=1
        assert(max:call(nil,4,8)==8)
        depth=0
        assert(thread.get_hook_storage()==storage)
    end
    if mode=="modify" then
        args[2]=sdk.to_ptr(19)
        return args
    end
    if mode=="skip" then return SKIP end
end,function(ret)
    if mode=="idle" then return ret end
    mark("a")
    assert(thread.get_hook_storage().depth==depth)
    if mode=="skip" then return sdk.to_ptr(sdk.to_int64(ret)+1) end
    return ret
end)
sdk.hook(max,function(args)
    if mode=="idle" then return end
    mark("B")
    assert(not next(thread.get_hook_storage()))
    if mode=="modify" then assert(sdk.to_int64(args[2])==19) end
    return CALL
end,function(ret)
    if mode=="idle" then return ret end
    mark("b")
    if mode=="skip" then return sdk.to_ptr(40) end
    return ret
end)
mode="original"
assert(max:call(nil,2,7)==7 and table.concat(trace)=="A0B0b0a0")
mode,trace="modify",{}
assert(max:call(nil,2,7)==19)
mode,trace="skip",{}
assert(max:call(nil,2,7)==41 and table.concat(trace)=="A0B0b0a0")
mode,trace="nested",{}
assert(max:call(nil,2,7)==7 and table.concat(trace)=="A0A1B1b1a1B0b0a0")
mode="idle"
assert(thread.get_hook_storage()==nil)
log.info("native hook passed: Int32, arg replacement, SKIP, ABba and recursion")

local arr=sdk.create_managed_array("System.Int32",2)
local setter=sdk.find_type_definition("System.Array"):get_method("SetValue(System.Object,System.Int32)")
arr[0]=13
local enabled=true
local post=0
sdk.hook(setter,function(args)
    if enabled and sdk.to_managed_object(args[2])==arr and sdk.to_int64(args[4])==0 then
        thread.get_hook_storage().skipped=true
        return SKIP
    end
end,function(ret)
    if enabled and thread.get_hook_storage().skipped then post=post+1 end
    return ret
end)
arr[0]=27
arr[1]=31
assert(post==1 and arr[0]==13 and arr[1]==31)
enabled=false
log.info("native hook passed: skipped native setter kept 13, normal setter wrote 31")

local floor=assert(math_type:get_method("Floor(System.Double)"))
local float_active=true
sdk.hook(floor,function(args)
    if float_active then
        assert(sdk.to_double(args[2])==2.75)
        args[2]=sdk.double_to_ptr(7.5)
    end
end,function(ret)
    if float_active then
        assert(sdk.to_double(ret)==7)
        return sdk.double_to_ptr(8.25)
    end
    return ret
end)
assert(floor:call(nil,2.75)==8.25)
float_active=false
log.info("native hook passed: Double argument and result in SIMD registers")

local absolute=assert(math_type:get_method("Abs(System.Int32)"))
local bad_pre,good_pre=0,0
local check_fault=true
sdk.hook(absolute,function(args)
    if check_fault then bad_pre=bad_pre+1;args[2]="bad Int32" end
end)
sdk.hook(absolute,function(args)
    if check_fault then
        assert((sdk.to_int64(args[2])&0xffffffff)==0xfffffffa)
        good_pre=good_pre+1
    end
end)
assert(absolute:call(nil,-6)==6 and absolute:call(nil,-6)==6)
check_fault=false
assert(bad_pre==1 and good_pre==2 and thread.get_hook_storage()==nil)
log.info("native hook passed: invalid subscriber disabled, arguments and second subscriber preserved")

local concat=assert(sdk.find_type_definition("System.String"):get_method("Concat(System.String,System.String)"))
local strings_active=true
sdk.hook(concat,function(args)
    if strings_active then args[2]=sdk.to_ptr(sdk.create_managed_string("雌火龙")) end
end,function(ret)
    if strings_active then
        local text=sdk.to_managed_object(ret)
        assert(text:get_Length()==6)
        return sdk.to_ptr(sdk.create_managed_string("上等鳞"))
    end
    return ret
end)
assert(concat:call(nil,"原文","的鳞片")=="上等鳞")
strings_active=false
collectgarbage("collect")
log.info("native hook passed: managed reference argument and return replacement")

local query=assert(sdk.find_type_definition("via.areamap.AreaQueryCore"):get_method("pathBetweenRegionsWithProperty(System.UInt32,System.UInt32,System.String,System.Boolean,System.Boolean,System.Boolean,System.Boolean,System.Boolean)"))
local query_active=true
local spill_pre,spill_post=0,0
local spill_ok,spill_error
sdk.hook(query,function(args)
    if not query_active then return end
    spill_ok,spill_error=pcall(function()
        assert(sdk.to_int64(args[2])==17 and sdk.to_int64(args[3])==29)
        assert(sdk.to_int64(args[5])==0 and sdk.to_int64(args[6])==1)
        assert(sdk.to_int64(args[7])==0 and sdk.to_int64(args[8])==1 and sdk.to_int64(args[9])==1)
    end)
    spill_pre=spill_pre+1
    return SKIP
end,function(ret)
    if query_active then spill_post=spill_post+1 end
    return ret
end)
assert(query:call(nil,17,29,"hook-only-query",false,true,false,true,true)==nil)
query_active=false
assert(spill_pre==1 and spill_post==1)
assert(spill_ok,spill_error)
log.info("native hook passed: overflow argument read from caller stack, query skipped")

local transform_type=sdk.find_type_definition("via.Transform")
local position=assert(transform_type:get_method("get_Position"))
local set_position=assert(transform_type:get_method("set_Position(via.vec3)"))
local inspecting=false
local position_calls=0
local target
sdk.hook(position,nil,function(ret)
    if inspecting then assert(ret);position_calls=position_calls+1 end
    return ret
end)
sdk.hook(set_position,function(args)
    if inspecting and sdk.to_managed_object(args[2])==target then return SKIP end
end,function(ret) return ret end)
local tested_transform=false
re.on_application_entry("UpdateScene",function()
    if tested_transform then return end
    local manager=sdk.get_managed_singleton("snow.player.PlayerManager")
    local player=manager and manager:findMasterPlayer()
    if not player then return end
    target=player:get_GameObject():get_Transform()
    local before=target:get_Position()
    inspecting=true
    target:set_Position(Vector3f.new(before.x+10,before.y+10,before.z+10))
    local after=target:get_Position()
    inspecting=false
    assert(position_calls==1 and before.x==after.x and before.y==after.y and before.z==after.z)
    tested_transform=true
    log.info("native hook passed: indirect vec3 argument and x8 return preserved, position setter skipped")
end)

-- No Lua call to update is made. These callbacks must originate in the engine.
local manager=assert(sdk.find_type_definition("snow.player.PlayerManager"))
local update=assert(manager:get_method("update"))
local calls=0
local threads={}
sdk.hook(update,function(args)
    local storage=thread.get_hook_storage()
    assert(not next(storage))
    storage.id=thread.get_id()
    threads[storage.id]=true
    storage.this=sdk.to_managed_object(args[2])
end,function(ret)
    local storage=thread.get_hook_storage()
    assert(storage.id==thread.get_id() and storage.this)
    calls=calls+1
    if calls==60 then log.info("native hook passed: 60 game-originated PlayerManager.update pre/post pairs") end
    return ret
end)
local motion=assert(sdk.find_type_definition("snow.player.PlayerMotionControl"):get_method("lateUpdate"))
local checked_threads={}
sdk.hook(motion,function()
    local id=thread.get_id()
    local storage=thread.get_hook_storage()
    storage.id=id
    if not checked_threads[id] then
        mode,trace,depth="nested",{},0
        local result=max:call(nil,2,7)
        mode="idle"
        assert(result==7 and table.concat(trace)=="A0A1B1b1a1B0b0a0")
        assert(thread.get_hook_storage()==storage)
    end
end,function(ret)
    local id=thread.get_id()
    assert(thread.get_hook_storage().id==id)
    if not checked_threads[id] then
        checked_threads[id]=true
        log.info("native hook passed: natural motion pre/post and nested thread storage on tid "..id)
    end
    return ret
end)
