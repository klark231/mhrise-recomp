-- Runs once on PlayerManager.update. The PC modules themselves are unmodified.
local function sample(label, fn)
    local ok, value = pcall(fn)
    log.info("yun " .. label .. " = " .. (ok and tostring(value) or ("ERROR " .. tostring(value))))
    if ok then return value end
end
local path = "yunwulian.yun_modules"
sample("full require", function() return type(require(path)) end)
for _, sub in ipairs({"constant","utils","core","player","action","input","state","slowmo","enemy","effects","derive","hooks","ui"}) do
    sample("require." .. sub,function() return type(require(path.."."..sub)) end)
end
local core, player = require(path..".core"), require(path..".player")
local state, input = require(path..".state"), require(path..".input")
core.init_singletons()
if not core.find_master_player() then log.info("yun: no active hunter"); return end
local p=core.master_player
sample("runtime player",function() return p:get_type_definition():get_full_name() end)
sample("core.update_game_data",function() core.update_game_data(); return "completed" end)
sample("player index",player.get_master_player_index)
sample("weapon",player.get_weapon_type)
if core.player_data then
    sample("attack",player.get_atk)
    sample("affinity",player.get_affinity)
    sample("player.get_vital",player.get_vital)
    for _, k in ipairs({"_vital","_vitalMax","_r_Vital","_stamina","_staminaMax"}) do
        sample(k,function() return core.player_data:get_field(k) end)
    end
end
sample("behavior node",function() return core._current_node end)
sample("motion bank",function() return p:getMotionBankID_Layer(0) end)
sample("motion id",function() return p:getMotionID_Layer(0) end)
sample("motion frame",core.get_current_frame)
sample("left stick",input.is_push_lstick)
sample("in quest",state.is_in_quest)
sample("paused",state.is_pausing)
sample("quest status",function() return sdk.get_managed_singleton("snow.QuestManager"):get_field("_QuestStatus") end)
sample("invulnerability timer",function() return core._muteki_time end)
sample("hyper armor timer",function() return core._hyper_armor_time end)
