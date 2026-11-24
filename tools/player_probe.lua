local manager = assert(sdk.get_managed_singleton("snow.player.PlayerManager"), "PlayerManager is unavailable")
local hunter = manager:call("findMasterPlayer()")
local name = hunter and hunter:get_type_definition():get_full_name() or "nil"
log.info(manager:get_type_definition():get_full_name() .. ".findMasterPlayer() => " .. name)
