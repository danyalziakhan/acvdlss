// ACV DLSS
//
// Settings, toggle key and overlay window. taa.cpp does the work.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "config.h"
#include "effects.h"
#include "frame_start.h"
#include "jitter.h"
#include "reflex.h"
#include "acvdlss.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <iterator>
#include <string>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "ACV DLSS";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Replaces Assassin's Creed Valhalla's anti-aliasing (TAA) with NVIDIA DLSS.";

settings g_settings;

static effect_runtime *g_runtime = nullptr;
static bool g_overlay_open = false;

// Index is NVIDIA's preset number. Dropped letters still go through as Auto.
static const char *PRESETS[] = {
    "Auto",
    "A (removed, acts as Auto)",
    "B (removed, acts as Auto)",
    "C (removed, acts as Auto)",
    "D (removed, acts as Auto)",
    "E (CNN)",
    "F (CNN)",
    "G (reserved, acts as Auto)",
    "H (reserved, acts as Auto)",
    "I (reserved, acts as Auto)",
    "J (transformer, 1st gen)",
    "K (transformer, 1st gen)",
    "L (transformer, 2nd gen)",
    "M (transformer, 2nd gen)",
};

//----------------------------------------------------------------------------
// Toggle key.

static int g_toggle_key = VK_F10;
static bool g_listening = false;
static int g_listen_frame = 0; // ImGui frame the key button was last drawn on

static void save_preset(uint32_t preset)
{
    const char letter[2] = {char('A' + preset - 1), '\0'};
    config_set("DLSS", "Preset", preset == 0 ? "Auto" : letter);
}

// Older versions saved NVIDIA's number. Migrated to the letter on read.
static uint32_t read_preset()
{
    const std::string text = config_string("DLSS", "Preset", "Auto");
    const char *value = text.c_str();
    if (value[0] >= '0' && value[0] <= '9')
    {
        const unsigned long number = strtoul(value, nullptr, 10);
        const uint32_t preset = number < std::size(PRESETS) ? uint32_t(number) : 0;
        save_preset(preset);
        return preset;
    }
    const char letter = char(toupper(static_cast<unsigned char>(value[0])));
    if (value[1] == '\0' && letter >= 'A' && letter < char('A' + std::size(PRESETS) - 1))
        return uint32_t(letter - 'A' + 1);
    return 0;
}

static void load_settings()
{
    config_open();
    g_settings.enabled = config_int("General", "Enabled", 1) != 0;
    g_settings.preset = read_preset();
    g_settings.sharpness = float(std::clamp(config_int("DLSS", "Sharpness", 0), 0, 100)) / 100.0f;
    g_settings.reflex = uint32_t(std::clamp(config_int("Latency", "Reflex", 1), 0, 2));
    g_settings.reflex_placement = uint32_t(std::clamp(config_int("Latency", "ReflexPlacement", 2), 0, 2));
    g_settings.hudless = config_int("OptiScaler", "HudlessImage", 1) != 0;
    const int positions = config_int("Advanced", "JitterPositions", 0); // Auto reads as 0
    g_settings.jitter_positions = positions == 8 || positions == 16 || positions == 32 ? uint32_t(positions) : 0;
    g_settings.vignette_mode = uint32_t(std::clamp(config_int("Image", "VignetteMethod", 0), 0, 1));
    g_settings.vignette_strength = float(std::clamp(config_int("Image", "VignetteStrength", 100), 0, 200)) / 100.0f;
    g_settings.debug = config_int("Diagnostics", "Debug", 0) != 0;

    const int key = config_int("General", "ToggleKey", VK_F10);
    g_toggle_key = key >= 0 && key < 256 ? key : VK_F10;
}

static std::string key_name(uint32_t vk);

static void log_settings()
{
    if (config_took_over())
        acv_log(reshade::log::level::info, "settings moved from ReShade.ini to acvdlss.ini");
    static const char *REFLEX[] = {"off", "on", "on + boost"};
    char positions[16] = "auto";
    if (g_settings.jitter_positions != 0)
        snprintf(positions, sizeof(positions), "%u", g_settings.jitter_positions.load());
    char text[320];
    snprintf(text, sizeof(text),
             "settings: %s, preset %s, sharpness %d%%, toggle key %s, Reflex %s, Reflex placement %u, HUD-less "
             "image %s, jitter positions %s%s",
             g_settings.enabled ? "on" : "off", PRESETS[g_settings.preset],
             int(std::lround(g_settings.sharpness * 100.0f)),
             g_toggle_key != 0 ? key_name(g_toggle_key).c_str() : "none", REFLEX[g_settings.reflex],
             g_settings.reflex_placement.load(), g_settings.hudless ? "on" : "off", positions,
             g_settings.debug ? ", Debug on" : "");
    acv_log(reshade::log::level::info, text);
}

static void save_toggle_key()
{
    config_set("General", "ToggleKey", g_toggle_key);
}

// Feedback for the toggle key with the overlay closed.
static const auto INDICATOR_TIME = std::chrono::seconds(3);
static std::chrono::steady_clock::time_point g_indicator_until;

static void log_change(const char *format, ...)
{
    char text[160];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    acv_log(reshade::log::level::info, text);
}

static void set_enabled(bool enabled, const char *how)
{
    log_change("DLSS switched %s with %s", enabled ? "on" : "off", how);
    g_settings.enabled = enabled;
    config_set("General", "Enabled", enabled ? 1 : 0);
    if (enabled)
        taa_restart();
    else
        jitter_restore(); // the game's TAA was made for its own 8 positions
    g_indicator_until = std::chrono::steady_clock::now() + INDICATOR_TIME;
}

static bool toggle_key_pressed()
{
    return g_toggle_key != 0 && g_runtime != nullptr && g_runtime->is_key_pressed(g_toggle_key);
}

// A key ReShade already uses would trigger both.
static bool is_reshade_key(int vk)
{
    for (const char *name : {"KeyOverlay", "KeyEffects", "KeyScreenshot", "KeyReload", "KeyNextPreset",
                             "KeyPreviousPreset", "KeyFPS", "KeyFrametime"})
    {
        char value[64] = "";
        size_t size = sizeof(value);
        unsigned key = 0;
        if (reshade::get_config_value(nullptr, "INPUT", name, value, &size) && sscanf(value, "%u", &key) == 1 &&
            int(key) == vk)
            return true;
    }
    return false;
}

static std::string utf8(const wchar_t *text)
{
    char out[64];
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof(out), nullptr, nullptr);
    return n > 0 ? std::string(out) : std::string();
}

// As printed on the key, in the player's layout.
static std::string key_name(uint32_t vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::string(1, char(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return "Numpad " + std::to_string(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24)
        return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk)
    {
    case VK_MBUTTON: return "Middle Mouse";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_PAUSE: return "Pause";
    case VK_CAPITAL: return "Caps Lock";
    case VK_ESCAPE: return "Escape";
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_TAB: return "Tab";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_END: return "End";
    case VK_HOME: return "Home";
    case VK_LEFT: return "Left";
    case VK_UP: return "Up";
    case VK_RIGHT: return "Right";
    case VK_DOWN: return "Down";
    case VK_SNAPSHOT: return "Print Screen";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_MULTIPLY: return "Numpad *";
    case VK_ADD: return "Numpad +";
    case VK_SUBTRACT: return "Numpad -";
    case VK_DECIMAL: return "Numpad .";
    case VK_DIVIDE: return "Numpad /";
    case VK_NUMLOCK: return "Num Lock";
    case VK_SCROLL: return "Scroll Lock";
    }
    if (const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); scan != 0)
    {
        wchar_t text[48] = L"";
        if (GetKeyNameTextW(LONG(scan << 16), text, int(std::size(text))) > 0)
            return utf8(text);
    }
    return "Key " + std::to_string(vk);
}

static void draw_toggle_key(effect_runtime *runtime)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine();
    const std::string name = g_toggle_key != 0 ? key_name(g_toggle_key) : std::string("None");
    const std::string label = (g_listening ? std::string("Press a key or Esc to cancel") : name) + "###toggle_key";
    const int frame = ImGui::GetFrameCount();
    // Window closed while listening. Don't wait unseen.
    if (g_listening && frame != g_listen_frame + 1)
        g_listening = false;
    bool started = false;
    if (ImGui::Button(label.c_str(), ImVec2(220.0f, 0.0f)) && !g_listening)
        g_listening = started = true;
    g_listen_frame = frame;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Switches DLSS on and off while playing.\nBackspace while picking leaves it without a key.");

    // Windows reports every key as up while the overlay has input.
    // Skip the first frame so the Space or Enter that pressed the button isn't taken.
    if (!g_listening || started || runtime == nullptr)
        return;
    // The left mouse button pressed the button.
    for (int vk = VK_MBUTTON; vk < 256; ++vk)
    {
        // The generic modifier codes repeat their left and right forms.
        if (!runtime->is_key_pressed(vk) || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
            continue;
        if (vk != VK_ESCAPE && is_reshade_key(vk))
            continue;
        if (vk != VK_ESCAPE)
        {
            g_toggle_key = vk == VK_BACK ? 0 : vk;
            save_toggle_key();
            log_change("toggle key set to %s", g_toggle_key != 0 ? key_name(g_toggle_key).c_str() : "none");
        }
        g_listening = false;
        break;
    }
}

//----------------------------------------------------------------------------
// The window.

static const ImVec4 WARNING_COLOR(0.96f, 0.73f, 0.24f, 1.0f);

static std::string describe_mode(const taa_status &s)
{
    char text[64];
    if (s.output_width == s.width && s.output_height == s.height)
        snprintf(text, sizeof(text), "DLAA %ux%u", s.width, s.height);
    else
        snprintf(text, sizeof(text), "%ux%u to %ux%u", s.width, s.height, s.output_width, s.output_height);
    return std::string(text) + (s.hdr ? ", HDR" : "");
}

// Also shows the bias a restart would apply if FSR changed since launch.
static void draw_lod_bias()
{
    static float wanted = lod_bias();
    static auto read = std::chrono::steady_clock::time_point();
    const auto now = std::chrono::steady_clock::now();
    if (now - read >= std::chrono::seconds(1))
    {
        wanted = lod_bias_saved();
        read = now;
    }
    if (lod_bias() == 0.0f && wanted == 0.0f)
        return;
    ImGui::TextDisabled("LOD bias: %+.2f", lod_bias());
    if (std::abs(wanted - lod_bias()) > 0.01f)
    {
        ImGui::SameLine();
        ImGui::TextColored(WARNING_COLOR, "(%+.2f after a restart)", wanted);
    }
}

static void draw_status(const taa_status &s)
{
    switch (s.state)
    {
    case taa_state::no_device:
        ImGui::TextDisabled("Waiting for the game to start rendering.");
        break;
    case taa_state::unavailable:
        ImGui::TextColored(WARNING_COLOR, "DLSS is unavailable: %s.", s.message.c_str());
        break;
    case taa_state::off:
        ImGui::TextDisabled("Off. The game's own anti-aliasing is running.");
        break;
    case taa_state::waiting:
        ImGui::TextDisabled("Waiting for the game's anti-aliasing pass.");
        ImGui::TextDisabled("Anti-aliasing has to be on High in the game's graphics options.");
        break;
    case taa_state::running:
        ImGui::Text("Running: %s.", describe_mode(s).c_str());
        if (s.camera_motion_only)
            ImGui::TextDisabled("This scene has no motion vectors. DLSS follows the camera alone.");
        break;
    case taa_state::failed:
        ImGui::TextColored(WARNING_COLOR, "DLSS stopped: %s.", s.message.c_str());
        ImGui::TextDisabled("The game's own anti-aliasing is back until the game restarts.");
        break;
    }
}

// Fixed so the controls don't move when DLSS is switched.
static const int STATUS_LINES = 3;

static void draw_window(effect_runtime *runtime)
{
    // Fits the longest status line at ReShade's default font size of 13.
    const float scale = ImGui::GetFontSize() / 13.0f;
    ImGui::SetWindowSize(ImVec2(560.0f * scale, 315.0f * scale), ImGuiCond_FirstUseEver);

    const float top = ImGui::GetCursorPosY();
    const taa_status s = taa_get_status();
    draw_status(s);
    draw_lod_bias();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), top + STATUS_LINES * ImGui::GetTextLineHeightWithSpacing()));
    ImGui::Separator();

    bool enabled = g_settings.enabled;
    if (ImGui::Checkbox("Enabled", &enabled))
        set_enabled(enabled, "the checkbox");
    draw_toggle_key(runtime);

    int preset = int(g_settings.preset);
    if (ImGui::Combo("Preset", &preset, PRESETS, int(std::size(PRESETS))))
    {
        g_settings.preset = uint32_t(preset);
        save_preset(uint32_t(preset));
        log_change("preset set to %s", PRESETS[preset]);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Auto lets DLSS choose the model NVIDIA recommends for your graphics card.");

    int sharpness = int(std::lround(g_settings.sharpness * 100.0f));
    if (ImGui::SliderInt("Sharpness", &sharpness, 0, 100, "%d%%"))
    {
        g_settings.sharpness = float(sharpness) / 100.0f;
        config_set("DLSS", "Sharpness", sharpness);
    }
    // Log once per drag.
    static int dragged_from = 0;
    if (ImGui::IsItemActivated())
        dragged_from = sharpness;
    if (ImGui::IsItemDeactivatedAfterEdit() && sharpness != dragged_from)
        log_change("sharpness set to %d%%", sharpness);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("AMD's RCAS on the DLSS output. It runs before the HUD is drawn.");

    static const char *REFLEX[] = {"Off", "On", "On + Boost"};
    const char *no_reflex = reflex_unavailable();
    ImGui::BeginDisabled(no_reflex != nullptr);
    int reflex = int(g_settings.reflex);
    if (ImGui::Combo("NVIDIA Reflex", &reflex, REFLEX, int(std::size(REFLEX))))
    {
        g_settings.reflex = uint32_t(reflex);
        config_set("Latency", "Reflex", reflex);
        log_change("Reflex set to %s", REFLEX[reflex]);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", no_reflex != nullptr
                                    ? no_reflex
                                    : "Cuts render queue latency. Input reaches the screen sooner.\n"
                                      "Boost keeps GPU clocks high when the CPU is the bottleneck. "
                                      "It uses more power.");

    static const char *VIGNETTES[] = {"Game's own", "Redrawn"};
    int vignette = int(g_settings.vignette_mode.load());
    if (ImGui::Combo("Vignette", &vignette, VIGNETTES, int(std::size(VIGNETTES))))
    {
        g_settings.vignette_mode = uint32_t(vignette);
        config_set("Image", "VignetteMethod", vignette);
        log_change("vignette set to %s", VIGNETTES[vignette]);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Game's own: the game draws it before DLSS. It can smear at the edges.\n"
                          "Redrawn: drawn again after DLSS to look like the game's own. It doesn't smear.");

    int strength = int(std::lround(g_settings.vignette_strength.load() * 100.0f));
    if (ImGui::SliderInt("Vignette strength", &strength, 0, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp))
        g_settings.vignette_strength = float(strength) / 100.0f;
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        config_set("Image", "VignetteStrength", strength);
        log_change("vignette strength set to %d%%", strength);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("100%% is the game's own. 0%% turns the vignette off.");
}

// Every frame, overlay open or not.
static void on_overlay(effect_runtime *)
{
    if (std::chrono::steady_clock::now() >= g_indicator_until)
        return;

    const taa_status s = taa_get_status();
    std::string text;
    ImVec4 color = WARNING_COLOR;
    if (!g_settings.enabled)
    {
        text = "DLSS: Off";
        color = ImVec4(0.95f, 0.35f, 0.3f, 1.0f);
    }
    else if (s.state == taa_state::running)
    {
        text = "DLSS: On (" + describe_mode(s) + ")";
        color = ImVec4(0.45f, 0.9f, 0.45f, 1.0f);
    }
    else if (s.state == taa_state::unavailable || s.state == taa_state::failed)
    {
        text = "DLSS: Unavailable. See the ACV DLSS window";
    }
    else
    {
        text = "DLSS: On. Waiting for the game's anti-aliasing";
    }

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##acvdlss_indicator", nullptr, flags))
    {
        ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 1.3f);
        ImGui::TextColored(color, "%s", text.c_str());
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

//----------------------------------------------------------------------------

static void on_init_effect_runtime(effect_runtime *runtime)
{
    g_runtime = runtime;
}

static void on_destroy_effect_runtime(effect_runtime *runtime)
{
    if (g_runtime == runtime)
        g_runtime = nullptr;
}

static bool on_open_overlay(effect_runtime *, bool open, input_source)
{
    g_overlay_open = open;
    return false;
}

static void on_present(effect_runtime *runtime)
{
    static uint32_t logged_width = 0, logged_height = 0;
    uint32_t width = 0, height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    if (width != logged_width || height != logged_height)
    {
        logged_width = width;
        logged_height = height;
        char text[64];
        snprintf(text, sizeof(text), "screen is %ux%u", width, height);
        acv_log(reshade::log::level::info, text);
    }

    // With the overlay open the key may be meant for a text box.
    if (runtime == g_runtime && !g_overlay_open && toggle_key_pressed())
        set_enabled(!g_settings.enabled, "the toggle key");
    taa_present(runtime->get_command_queue(), width, height);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        // Must be the first ReShade call.
        // reshade.hpp latches the module handle on first use and only register_addon passes it.
        if (!reshade::register_addon(module))
            return FALSE;
        load_settings();
        log_start();
        log_settings();
        jitter_start();
        lod_register();
        taa_register();
        reflex_register();
        effects_register();
        if (g_settings.reflex_placement != uint32_t(reflex_placement::after_present))
            frame_start_hook();
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(on_open_overlay);
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_overlay);
        reshade::register_overlay("ACV DLSS", draw_window);
        break;
    case DLL_PROCESS_DETACH:
        // TAA first. Then no frame can change the jitter after the restore.
        taa_unregister();
        effects_unregister();
        reflex_unregister();
        jitter_restore();
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
