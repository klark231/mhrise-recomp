assert(sdk.get_tdb_version() == 71)
for _, pair in ipairs({
    {"snow.player.PlayerManager", "findMasterPlayer"},
    {"snow.player.ShortSword", "lateUpdate"},
    {"via.SceneManager", "get_CurrentScene"},
}) do
    local t = assert(sdk.find_type_definition(pair[1]), pair[1])
    local method = assert(t:get_method(pair[2]), pair[2])
    log.info(t:get_full_name() .. "." .. method:get_name())
    assert(t:get_method("MissingMethodForProbe") == nil)
end
assert(sdk.find_type_definition("Missing.TypeForProbe") == nil)
local weapon = assert(sdk.find_type_definition("snow.player.PlayerWeaponCtrl"))
assert(weapon:get_name() == "PlayerWeaponCtrl" and weapon:get_namespace() == "snow.player")
local scale = assert(weapon:get_field("_bodyConstScale"))
assert(not scale:is_static() and scale:get_type():get_full_name() == "System.Single")
assert(scale:get_declaring_type():get_full_name() == weapon:get_full_name())
assert(#weapon:get_fields() > 0 and weapon:get_parent_type() ~= nil)
local damage = assert(sdk.find_type_definition("snow.player.PlayerQuestBase"):get_method(
    "calcBloodySkillHealVital(snow.hit.EnemyCalcDamageInfo.AfterCalcInfo_DamageSide, System.Boolean)"))
assert(damage:get_num_params() == 2 and damage:get_return_type():get_full_name() == "System.Void")
assert(damage:get_param_types()[2]:get_full_name() == "System.Boolean")
local damage_info = assert(sdk.find_type_definition("snow.hit.EnemyCalcDamageInfo.AfterCalcInfo_DamageSide"))
assert(damage_info:get_field("<IsBreakPartsDamage>k__BackingField"):get_type():get_full_name() == "System.Boolean")
log.info("TDB71 Lua probe passed")
