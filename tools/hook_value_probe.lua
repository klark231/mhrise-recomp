-- Runs against the game's own boxed reflection invoker and native methods.
local counter = "snow.gui.fsm.questcounter.GuiQuestCounterFsmManager"
for _, spec in ipairs({
    {"snow.SnowSingletonBehaviorRoot`1", "awake"},
    {"snow.gui.fsm.GuiFsmBaseManager`1", "onDestroy"}
}) do
    local name = spec[1] .. "<" .. counter .. ">"
    local closed = assert(sdk.find_type_definition(name), name)
    assert(closed:get_full_name() == name and closed:get_method(spec[2]))
end
local filter = assert(sdk.find_type_definition("via.network.session.SearchFilter"))
assert(filter:get_method("pushBackU32Filter"):get_num_params() == 3)
log.info("hook value probe passed: closed generic hook owners and bare overload lookup")
local vec = assert(sdk.find_type_definition("via.vec3"))
local quat = assert(sdk.find_type_definition("via.Quaternion"))
local camera = assert(sdk.find_type_definition("snow.camera.CameraUtility"))
local rotate = assert(camera:get_method("RotVecX(via.vec3,System.Single)"))
local mode, calls = nil, 0
sdk.hook(rotate, function(args)
    if not mode then return end
    assert(args[2] == sdk.get_thread_context())
    assert(sdk.get_native_field(args[3], vec, "x") == 2)
    assert(not pcall(sdk.get_native_field, args[3], quat, "x"))
    local storage = thread.get_hook_storage()
    storage.output, storage.input = args[1], args[3]
    sdk.set_native_field(args[3], vec, "x", 11)
end, function(ret)
    if not mode then return ret end
    local storage = thread.get_hook_storage()
    assert(ret == storage.output and sdk.get_native_field(ret, vec, "x") == 11)
    calls = calls + 1
    sdk.set_native_field(ret, vec, "z", 17)
    if mode == "copy" then return storage.input end
    return ret
end)
for _, test in ipairs({"field", "copy"}) do
    mode = test
    local result = rotate:call(nil, Vector4f.new(2, 3, 4, 0), 0)
    mode = nil
    assert(result.x == 11 and result.y == 3 and result.z == (test == "copy" and 4 or 17))
end
assert(calls == 2)
log.info("hook value probe passed: aggregate result slot, in-place vec3 fields and typed return copy")

local norm = assert(sdk.find_type_definition("via.quaternion"):get_method("normalize(via.Quaternion)"))
local active = true
sdk.hook(norm, function(args)
    if not active then return end
    assert(sdk.get_native_field(args[3], quat, "w") == 1)
    sdk.set_native_field(args[3], quat, "w", 0)
    sdk.set_native_field(args[3], quat, "x", 2)
end)
local q = norm:call(nil, Quaternion.identity())
active = false
assert(math.abs(q.x - 1) < 0.0001 and q.w == 0)
log.info("hook value probe passed: quaternion input reached original method")

local gui = assert(sdk.get_managed_singleton("snow.gui.GuiManager"))
local quest = assert(sdk.get_managed_singleton("snow.QuestManager"))
local gui_method = assert(sdk.find_type_definition("snow.gui.GuiManager"):get_method("get_IsUnLockSkillDetaiWindowInSmithy"))
local quest_method = assert(sdk.find_type_definition("snow.QuestManager"):get_method("get_isFirstClearMaxRandomMysteryQuest"))
local shared_entry = assert(gui_method:get_function())
assert(quest_method:get_function() == shared_entry)
assert(sdk.is_managed_object(gui) and sdk.is_managed_object(quest:get_address()))
assert(not sdk.is_managed_object(nil) and not sdk.is_managed_object(sdk.to_ptr(1)))
assert(not sdk.is_managed_object(shared_entry))
local trace, testing = {}, false
local function check(method, receiver, label)
    sdk.hook(method, function(args)
        if not testing then return end
        assert(sdk.is_managed_object(args[2]))
        assert(sdk.to_managed_object(args[2]) == receiver)
        trace[#trace + 1] = label
    end, function(ret)
        if testing then trace[#trace + 1] = label end
        return ret
    end)
end
check(gui_method, gui, "G")
check(quest_method, quest, "Q")
testing = true
gui_method:call(gui)
quest_method:call(quest)
testing = false
assert(table.concat(trace) == "GGQQ", table.concat(trace))
log.info("hook value probe passed: shared getter dispatched only the matching receiver GGQQ")
