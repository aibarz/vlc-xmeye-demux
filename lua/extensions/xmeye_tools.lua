-- SPDX-License-Identifier: LGPL-2.1-or-later
-- XMEye Tools control extension for VLC 3.x
-- Original concept and functional design: Álvaro Ibarz
-- Copyright (C) 2026 Álvaro Ibarz

local dlg = nil
local temporal_enabled = nil
local adaptive_enabled = nil
local strength = nil
local threshold_y = nil
local threshold_c = nil
local status = nil

local DEFAULT_STRENGTH = 4
local DEFAULT_THRESHOLD_Y = 12
local DEFAULT_THRESHOLD_C = 8

function descriptor()
    return {
        title = "XMEye Tools",
        version = "0.1.0",
        author = "Álvaro Ibarz",
        shortdesc = "XMEye temporal and zoom controls",
        description = "Live controls for the XMEye VLC plugins. Concept and functional design by Álvaro Ibarz.",
        capabilities = {}
    }
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

function apply_temporal()
    local vout = get_vout()
    if not vout then return end

    local sy = tonumber(threshold_y:get_text())
    local sc = tonumber(threshold_c:get_text())
    local s = tonumber(strength:get_value()) or DEFAULT_STRENGTH

    if not sy or sy < 0 or sy > 64 then
        set_status("Umbral Y inválido: usa 0–64.")
        return
    end
    if not sc or sc < 0 or sc > 64 then
        set_status("Umbral C inválido: usa 0–64.")
        return
    end

    s = math.max(2, math.min(8, math.floor(s)))
    sy = math.floor(sy)
    sc = math.floor(sc)

    local values = {
        { "xmeye2x-enabled", temporal_enabled:get_checked() },
        { "xmeye2x-adaptive", adaptive_enabled:get_checked() },
        { "xmeye2x-strength", s },
        { "xmeye2x-threshold-y", sy },
        { "xmeye2x-threshold-c", sc }
    }

    for _, item in ipairs(values) do
        local ok, err = set_live(vout, item[1], item[2])
        if not ok then
            set_status("Error aplicando " .. item[1] .. ": " .. err)
            return
        end
    end

    local mode = temporal_enabled:get_checked() and "ON" or "OFF"
    local adaptive = adaptive_enabled:get_checked() and "adaptativo" or "promedio simple"
    set_status(string.format("Temporal %s · %s · fuerza %d · Y %d · C %d", mode, adaptive, s, sy, sc))
end

function reset_temporal()
    temporal_enabled:set_checked(true)
    adaptive_enabled:set_checked(true)
    threshold_y:set_text(tostring(DEFAULT_THRESHOLD_Y))
    threshold_c:set_text(tostring(DEFAULT_THRESHOLD_C))

    local vout = get_vout()
    if vout then
        set_live(vout, "xmeye2x-enabled", true)
        set_live(vout, "xmeye2x-adaptive", true)
        set_live(vout, "xmeye2x-strength", DEFAULT_STRENGTH)
        set_live(vout, "xmeye2x-threshold-y", DEFAULT_THRESHOLD_Y)
        set_live(vout, "xmeye2x-threshold-c", DEFAULT_THRESHOLD_C)
    end

    set_status("Valores temporales restablecidos: fuerza 4 · Y 12 · C 8")
end

function activate()
    dlg = vlc.dialog("XMEye Tools")

    dlg:add_label("Filtro temporal 2→1", 1, 1, 4, 1)
    temporal_enabled = dlg:add_check_box("Activado", true, 1, 2, 2, 1)
    adaptive_enabled = dlg:add_check_box("Acumulación adaptativa", true, 3, 2, 2, 1)

    dlg:add_label("Fuerza temporal", 1, 3, 2, 1)
    strength = dlg:add_dropdown(3, 3, 2, 1)
    strength:add_value("4 — recomendado", 4)
    strength:add_value("2 — suave", 2)
    strength:add_value("3", 3)
    strength:add_value("5", 5)
    strength:add_value("6", 6)
    strength:add_value("7", 7)
    strength:add_value("8 — máximo", 8)

    dlg:add_label("Umbral luminancia Y (0–64)", 1, 4, 2, 1)
    threshold_y = dlg:add_text_input(tostring(DEFAULT_THRESHOLD_Y), 3, 4, 2, 1)

    dlg:add_label("Umbral color C (0–64)", 1, 5, 2, 1)
    threshold_c = dlg:add_text_input(tostring(DEFAULT_THRESHOLD_C), 3, 5, 2, 1)

    dlg:add_button("Aplicar", apply_temporal, 1, 6, 2, 1)
    dlg:add_button("Valores por defecto", reset_temporal, 3, 6, 2, 1)

    dlg:add_label("Zoom interactivo: se añadirá en el siguiente módulo xmeyezoom.", 1, 7, 4, 1)
    status = dlg:add_label("Listo. Los cambios se aplican al vídeo activo.", 1, 8, 4, 1)
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
