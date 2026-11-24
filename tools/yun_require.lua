local yun = require("yunwulian.yun_modules")
assert(type(yun.get_vital) == "function")
log.info("yun_modules entry point loaded")
local changes = 0
yun.on_action_change(function()
    if not yun.get_master_player() or changes >= 12 then return end
    changes = changes + 1
    log.info(("yun action bank=%s id=%s frame=%s node=%s"):format(
        yun.get_action_bank_id(),yun.get_action_id(),yun.get_now_action_frame(),yun.get_current_node()))
end)
local sampled = false
re.on_application_entry("UpdateScene", function()
    if sampled or not yun.get_master_player() or not yun.get_weapon_type() then return end
    local camera = sdk.get_primary_camera()
    if not camera then return end
    sampled = true
    log.info(("yun live clock=%.2f delta=%.4f timescale=%.2f cameraFov=%.2f skill1=%d book=%d"):format(
        yun.get_time(),yun.get_delta_time(),yun.get_player_timescale(),camera:get_FOV(),
        yun.check_equip_skill_lv(1),yun.get_selected_book()))
end)
