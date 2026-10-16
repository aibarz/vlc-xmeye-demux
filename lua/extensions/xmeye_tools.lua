-- SPDX-License-Identifier: LGPL-2.1-or-later
-- XMEye Tools control extension for VLC 3.x
-- Original concept and functional design: Álvaro Ibarz
-- Copyright (C) 2026 Álvaro Ibarz

local dlg = nil
local profile = nil
local block_size = nil
local focus_mode = nil
local focus_strength = nil
local focus_activity = nil
local focus_limit = nil
local status = nil

local function descriptor_values()
    return {
        title = "XMEye Tools",
        version = "0.2.0",
        author = "Álvaro Ibarz",
        shortdesc = "XMEye enhancement, focus and zoom controls",
        description = "Live controls for the XMEye VLC plugins. Original concept and functional design by Álvaro Ibarz.",
        capabilities = {}
    }
end

function descriptor()
    return descriptor_values()
end

local function set_status(text)
    if status then
        status:set_text(text)
        dlg:update()
    end
end

local function get_vout()
    local vout = vlc.object.vout()
    if not vout then
        set_status("No hay salida de vídeo activa.")
        return nil
    end
    return vout
end

local function set_live(vout, name, value)
    local ok, err = pcall(vlc.var.set, vout, name, value)
    if not ok then
        return false, tostring(err)
    end
    return true, nil
end

local function set_checked_filter(vout, name, value)
    local ok, err = set_live(vout, name, value)
    if not ok then
        set_status("Error en " .. name .. ": " .. err)
        return false
    end
    return true
end

local function selected_profile()
    return tonumber(profile:get_value()) or 0
end

local function profile_text(p)
    if p == 0 then return "ORIGINAL" end
    if p == 1 then return "STATIC 2→1" end
    if p == 2 then return "HYBRID 25" end
    if p == 3 then return "HYBRID 50/50" end
    if p == 4 then return "HYBRID 75" end
    if p == 5 then return "MOTION MC" end
    return "DESCONOCIDO"
end

function apply_profile()
    local vout = get_vout()
    if not vout then return end

    local p = selected_profile()
    local bs = tonumber(block_size:get_value()) or 16

    -- One enhancement engine at a time. All modules remain loaded in the chain.
    if not set_checked_filter(vout, "xmeye2x-enabled", p == 1) then return end
    if not set_checked_filter(vout, "xmeyemc-enabled", p == 5) then return end
    if not set_checked_filter(vout, "xmeyehybrid-enabled", p >= 2 and p <= 4) then return end

    -- Static 2→1 deliberately uses the non-recursive pair average.
    set_live(vout, "xmeye2x-adaptive", false)
    set_live(vout, "xmeye2x-strength", 2)

    set_live(vout, "xmeyemc-block-size", bs)
    set_live(vout, "xmeyehybrid-block-size", bs)

    if p == 2 then set_live(vout, "xmeyehybrid-focus", 25) end
    if p == 3 then set_live(vout, "xmeyehybrid-focus", 50) end
    if p == 4 then set_live(vout, "xmeyehybrid-focus", 75) end

    set_status("Perfil aplicado: " .. profile_text(p) .. " · bloque " .. tostring(bs) .. "×" .. tostring(bs))
end

local function focus_preset(mode)
    if mode == 0 then return false, 0, 5, 8, "OFF" end
    if mode == 1 then return true, 25, 6, 6, "LOW" end
    if mode == 2 then return true, 40, 5, 9, "MEDIUM" end
    if mode == 3 then return true, 60, 4, 12, "HIGH" end
    return false, 0, 5, 8, "OFF"
end

function apply_focus()
    local vout = get_vout()
    if not vout then return end

    local mode = tonumber(focus_mode:get_value()) or 0
    local enabled, strength, activity, limit, name = focus_preset(mode)

    -- Expert fields override preset when valid and non-empty.
    local s = tonumber(focus_strength:get_text())
    local a = tonumber(focus_activity:get_text())
    local l = tonumber(focus_limit:get_text())
    if s then strength = math.max(0, math.min(100, math.floor(s))) end
    if a then activity = math.max(0, math.min(32, math.floor(a))) end
    if l then limit = math.max(1, math.min(32, math.floor(l))) end

    set_live(vout, "xmeyefocus-enabled", enabled)
    set_live(vout, "xmeyefocus-strength", strength)
    set_live(vout, "xmeyefocus-activity-threshold", activity)
    set_live(vout, "xmeyefocus-correction-limit", limit)

    set_status(string.format("Focus %s · fuerza %d · actividad %d · límite %d", name, strength, activity, limit))
end

function load_focus_preset()
    local mode = tonumber(focus_mode:get_value()) or 0
    local enabled, strength, activity, limit, name = focus_preset(mode)
    focus_strength:set_text(tostring(strength))
    focus_activity:set_text(tostring(activity))
    focus_limit:set_text(tostring(limit))
    set_status("Preset Focus preparado: " .. name .. " · pulsa Aplicar focus")
end

function activate()
    dlg = vlc.dialog("XMEye Tools")

    dlg:add_label("MEJORA TEMPORAL / MOVIMIENTO", 1, 1, 4, 1)
    dlg:add_label("Perfil", 1, 2, 1, 1)
    profile = dlg:add_dropdown(2, 2, 3, 1)
    profile:add_value("Original — sin mejora temporal", 0)
    profile:add_value("Static 2→1 — 10 fps, promedio por parejas", 1)
    profile:add_value("Hybrid 25 — prioridad estática", 2)
    profile:add_value("Hybrid 50/50 — equilibrado", 3)
    profile:add_value("Hybrid 75 — prioridad movimiento", 4)
    profile:add_value("Motion MC — solo movimiento", 5)

    dlg:add_label("Bloque", 1, 3, 1, 1)
    block_size = dlg:add_dropdown(2, 3, 1, 1)
    block_size:add_value("16×16", 16)
    block_size:add_value("8×8", 8)
    block_size:add_value("24×24", 24)
    block_size:add_value("32×32", 32)
    dlg:add_button("Aplicar perfil", apply_profile, 3, 3, 2, 1)

    dlg:add_label("CORRECCIÓN DE FOCO", 1, 5, 4, 1)
    focus_mode = dlg:add_dropdown(1, 6, 2, 1)
    focus_mode:add_value("OFF", 0)
    focus_mode:add_value("LOW", 1)
    focus_mode:add_value("MEDIUM", 2)
    focus_mode:add_value("HIGH", 3)
    dlg:add_button("Cargar preset", load_focus_preset, 3, 6, 2, 1)

    dlg:add_label("Fuerza 0–100", 1, 7, 1, 1)
    focus_strength = dlg:add_text_input("25", 2, 7, 1, 1)
    dlg:add_label("Actividad 0–32", 3, 7, 1, 1)
    focus_activity = dlg:add_text_input("6", 4, 7, 1, 1)

    dlg:add_label("Límite 1–32", 1, 8, 1, 1)
    focus_limit = dlg:add_text_input("6", 2, 8, 1, 1)
    dlg:add_button("Aplicar focus", apply_focus, 3, 8, 2, 1)

    dlg:add_label("ZOOM", 1, 10, 4, 1)
    dlg:add_label("xmeyezoom: en desarrollo — zoom fijo, centro X/Y y aumento configurables.", 1, 11, 4, 1)

    status = dlg:add_label("Listo. Los cambios se aplican al vídeo activo.", 1, 13, 4, 1)
    dlg:show()
end

function deactivate()
    if dlg then
        dlg:delete()
        dlg = nil
    end
end

function close()
    vlc.deactivate()
end
