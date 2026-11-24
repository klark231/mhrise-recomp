-- Exercise the installed PC modules in the resident game VM.
local root = "yunwulian.yun_modules"
local loaded = 0
for _, name in ipairs({"constant","core","utils","player","action","input","state","effects","slowmo","derive","hooks","ui","enemy"}) do
    local ok, result = pcall(require, root .. "." .. name)
    if ok then loaded = loaded + 1 else log.error(name .. ": " .. tostring(result)) end
end
log.info(("yun resident submodules %d/13"):format(loaded))
local core, player = require(root .. ".core"), require(root .. ".player")
core.init_singletons()
local frames, first_player = 0, nil
local snapshot = "Waiting for hunter"
re.on_application_entry("UpdateScene", function()
    if frames >= 120 or not core.find_master_player() then return end
    frames = frames + 1
    if frames == 1 then first_player = core.master_player end
    if frames == 1 or frames == 120 then
        core.update_game_data()
        snapshot = ("weapon=%s HP=%s ATK=%s affinity=%s node=%s"):format(
            player.get_weapon_type(),player.get_vital(),player.get_atk(),player.get_affinity(),core._current_node)
        log.info(("yun frame %d same_hunter=%s %s"):format(frames,tostring(first_player == core.master_player),snapshot))
    end
end)
re.on_draw_ui(function()
    imgui.text("Yun Modules")
    imgui.text(snapshot)
    if imgui.button("Log sample") then log.info("MOD UI touched: " .. snapshot) end
end)
re.on_script_reset(function() log.info("yun resident reset at frame " .. frames) end)
