-- Stage with stage-live. Only the calls made by this probe change an argument.
local player_type = assert(sdk.find_type_definition("snow.player.PlayerBase"))
local adjust = assert(player_type:get_method("getAdjustTotalStaminaAttack(System.Single)"))
local probe
local item_name

sdk.hook(adjust, function(args)
    if not probe then return end
    local storage = thread.get_hook_storage()
    assert(storage and not next(storage))
    assert(thread.get_id() == probe.tid and thread.get_hash() == probe.hash)
    assert(args[1] == sdk.get_thread_context() and args[2] == probe.player)
    storage.phase = probe.phase
    probe.first_pre = probe.first_pre + 1
    if probe.phase == "outer" then
        assert(args[3] == 9)
        probe.phase = "inner"
        probe.inner_result = adjust:call(probe.player, 17)
        probe.phase = "outer"
        assert(thread.get_hook_storage() == storage and storage.phase == "outer")
        args[3] = 17
    end
end, function(result)
    if probe then
        assert(thread.get_hook_storage().phase == probe.phase)
        probe.first_post = probe.first_post + 1
    end
    return result
end)

sdk.hook(adjust, function(args)
    if not probe then return end
    local storage = thread.get_hook_storage()
    assert(not next(storage) and args[3] == 17)
    storage.owner = "second"
    probe.second_pre = probe.second_pre + 1
end, function(result)
    if probe then
        assert(thread.get_hook_storage().owner == "second")
        probe.second_post = probe.second_post + 1
    end
    return result
end)

local scene_tid, worker_tid
local function test_call(enclosing)
    local manager = sdk.get_managed_singleton("snow.player.PlayerManager")
    local player = manager and manager:findMasterPlayer()
    if not player or not player:get_PlayerSkillList() then return end
    local reference = adjust:call(player, 17)
    local run = {tid=thread.get_id(), hash=thread.get_hash(), player=player, phase="outer",
                 first_pre=0, first_post=0, second_pre=0, second_post=0}
    probe = run
    local ok, result = pcall(adjust.call, adjust, player, 9)
    probe = nil
    assert(ok, result)
    assert(run.first_pre == 2 and run.first_post == 2 and run.second_pre == 2 and run.second_post == 2)
    assert(thread.get_hook_storage() == enclosing and reference == run.inner_result and result == reference)
    log.info(("hook/thread passed: tid=%d; nested calls paired; arg 9 -> 17; result=%g"):format(run.tid,result))
    return run.tid
end
re.on_application_entry("UpdateScene", function()
    if scene_tid then return end
    assert(thread.get_hook_storage() == nil)
    scene_tid = test_call(nil)
end)
local late_update = assert(sdk.find_type_definition("snow.player.PlayerMotionControl"):get_method("lateUpdate"))
sdk.hook(late_update, function()
    if not scene_tid or worker_tid or thread.get_id() == scene_tid then return end
    worker_tid = test_call(assert(thread.get_hook_storage()))
    if worker_tid then log.info(("hook/thread verified on game threads %d and %d"):format(scene_tid,worker_tid)) end
end)

-- Fetch the same localized item label used by Better Than Shopping.
local item_id = 0x04100000
local get_name = assert(sdk.find_type_definition("snow.data.DataShortcut"):get_method("getName(snow.data.ContentsIdSystem.ItemId)"))
re.on_application_entry("UpdateScene", function()
    if not scene_tid or item_name or item_id>0x04100F00 then return end
    for _=1,32 do
        local name=get_name:call(nil,item_id)
        if name=="雌火龙的上等鳞" then
            item_name=name
            log.info(("font probe: game item 0x%08X = %s"):format(item_id,name))
            return
        end
        item_id=item_id+1
        if item_id>0x04100F00 then return end
    end
end)
re.on_draw_ui(function()
    if item_name then imgui.text(item_name) end
end)
