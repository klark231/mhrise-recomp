-- Writes MH Rise's saves without encryption, so they can be converted to another platform.
-- For the PC game (the Microsoft Store / Game Pass one above all, whose own encryption nothing else opens), with
-- REFramework: copy this into the game folder's reframework/autorun/, then in the REFramework menu open Script
-- Generated UI → "Rise: save without encryption". The port imports the plain save (Settings → Saves → Import a save).
--
-- The game's save service (via.storage.saveService.SaveService) has two settings for this:
--   SaveEncryptionType      None, AutoStrong, XOR, BlowFish, Citrus, Lime, RdsModule: which encryption
--   SaveEncryptionPlatform  None, AutoUse, All: on which platforms it's used at all
-- With only the type set to None the Microsoft Store game still wrote its usual format (the platform's own
-- encryption comes from AutoUse), so the button sets both. The next save the game makes goes out as plain data.
-- Back up your save folder first. Nothing changes until you press the button, and nothing stays after the game closes.

local service_type = sdk.find_type_definition("via.storage.saveService.SaveService")
local status = ""

local types = {[0] = "None", "AutoStrong", "XOR", "BlowFish", "Citrus", "Lime", "RdsModule"}
local platforms = {[0] = "None", "AutoUse", "All"}

local function service()
    return sdk.get_native_singleton("via.storage.saveService.SaveService")
end

local function call(name, ...)
    local s = service()
    if s == nil or service_type == nil then return nil end
    local ok, value = pcall(sdk.call_native_func, s, service_type, name, ...)
    if ok then return value end
    status = name .. " failed: " .. tostring(value)
    return nil
end

local function named(list, value)
    if value == nil then return "?" end
    return tostring(value) .. " (" .. (list[value] or "?") .. ")"
end

local function show()
    return "type " .. named(types, call("get_SaveEncryptionType")) .. ", platform " ..
               named(platforms, call("get_SaveEncryptionPlatform")) .. ", ForceWindows " ..
               tostring(call("get_ForceWindows"))
end

re.on_draw_ui(function()
    if not imgui.tree_node("Rise: save without encryption") then return end
    if service() == nil then
        imgui.text("The save service isn't up yet (wait for the title screen).")
    else
        imgui.text("Encryption type: " .. named(types, call("get_SaveEncryptionType")))
        imgui.text("Encryption platform: " .. named(platforms, call("get_SaveEncryptionPlatform")))
        imgui.text("ForceWindows: " .. tostring(call("get_ForceWindows")))
        if imgui.button("Turn encryption off (type and platform = None)") then
            call("set_SaveEncryptionType", 0)
            call("set_SaveEncryptionPlatform", 0)
            status = "Set. Now let the game save (load your hunter, then finish or abandon a quest, or change a setting)."
            log.info("[rise_save_plain] after the button: " .. show())
        end
    end
    if status ~= "" then imgui.text(status) end
    imgui.tree_pop()
end)

-- What the game had at start, in re2_framework_log.txt (once the service is up)
local logged = false
re.on_frame(function()
    if logged or service() == nil then return end
    logged = true
    log.info("[rise_save_plain] at start: " .. show())
end)
