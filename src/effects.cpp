// update_texture_bindings makes ReShade wait for the GPU to go idle when a loaded effect uses that semantic.
// Textures are bound only when they change: DLSS starting or stopping, or a resize.
//
// The HUD-less image costs a copy a frame at DLAA. It's only made while a loaded effect declares it.
// ReShade can't list the semantics effects use but does report what a texture is bound to.
// A 1x1 placeholder is bound. Any texture reporting the placeholder declares the semantic.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "acvdlss.h"
#include "effects.h"

#include <atomic>
#include <mutex>

using namespace reshade::api;

namespace
{
const char *const MOTION = "ACVDLSS_MOTION";
const char *const DEPTH = "ACVDLSS_DEPTH";
const char *const HUDLESS = "ACVDLSS_HUDLESS";

struct bound_texture
{
    ID3D12Resource *resource = nullptr;
    resource_view view = {0};
};

std::mutex g_lock;
effect_runtime *g_runtime = nullptr;
bound_texture g_motion, g_depth, g_hudless;
resource g_placeholder = {0};
// Read by the DLSS pass on another thread.
std::atomic<bool> g_hudless_used{false};

void destroy_view(device *dev, resource_view &view)
{
    if (view.handle != 0)
        dev->destroy_resource_view(view);
    view = {0};
}

void release_all(device *dev)
{
    destroy_view(dev, g_motion.view);
    destroy_view(dev, g_depth.view);
    destroy_view(dev, g_hudless.view);
    if (g_placeholder.handle != 0)
        dev->destroy_resource(g_placeholder);
    g_placeholder = {0};
    g_motion.resource = g_depth.resource = g_hudless.resource = nullptr;
}

ID3D12Resource *placeholder(device *dev)
{
    if (g_placeholder.handle == 0)
    {
        uint32_t black = 0;
        const subresource_data data = {&black, 4, 4};
        if (!dev->create_resource(resource_desc(1, 1, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::default_,
                                                resource_usage::shader_resource),
                                  &data, resource_usage::shader_resource, &g_placeholder))
            g_placeholder = {0};
    }
    return reinterpret_cast<ID3D12Resource *>(g_placeholder.handle);
}

void bind(device *dev, const char *semantic, bound_texture &bound, ID3D12Resource *resource, format fmt)
{
    if (resource == bound.resource)
        return;
    resource_view view = {0};
    if (resource != nullptr &&
        !dev->create_resource_view({reinterpret_cast<uint64_t>(resource)}, resource_usage::shader_resource,
                                   resource_view_desc(fmt), &view))
        view = {0};
    // A zero view binds ReShade's empty texture.
    g_runtime->update_texture_bindings(semantic, view, view);
    destroy_view(dev, bound.view);
    bound = {view.handle != 0 ? resource : nullptr, view};
}

void on_init_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard lock(g_lock);
    if (g_runtime != nullptr)
        release_all(g_runtime->get_device());
    g_runtime = runtime;
}

// After effects compile: whether any of them declares the HUD-less texture.
void on_reloaded_effects(effect_runtime *runtime)
{
    std::lock_guard lock(g_lock);
    if (runtime != g_runtime || g_hudless.view.handle == 0)
        return;
    bool used = false;
    runtime->enumerate_texture_variables(nullptr, [&used](effect_runtime *rt, effect_texture_variable variable) {
        resource_view srv = {0};
        rt->get_texture_binding(variable, &srv, nullptr);
        used |= srv.handle == g_hudless.view.handle;
    });
    if (used != g_hudless_used)
        acv_log(reshade::log::level::info, used ? "keeping the HUD-less image for an effect that reads it"
                                                : "no effect reads the HUD-less image");
    g_hudless_used = used;
}

void on_destroy_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard lock(g_lock);
    if (g_runtime != runtime)
        return;
    g_runtime = nullptr;
    release_all(runtime->get_device());
}

// Runs after this frame's effects. Binds the textures of the frame DLSS ran in.
// Binds nothing once DLSS stops so effects don't read old motion in menus.
void on_present(effect_runtime *runtime)
{
    shared_textures shared;
    if (!taa_shared_textures(shared))
        shared = {};
    ID3D12Resource *const motion = shared.motion, *const depth = shared.depth;
    std::lock_guard lock(g_lock);
    if (runtime != g_runtime)
        return;
    device *dev = runtime->get_device();
    bind(dev, MOTION, g_motion, motion, format::r16g16_float);
    bind(dev, DEPTH, g_depth, depth, format::r32_float);
    if (shared.hudless != nullptr)
        bind(dev, HUDLESS, g_hudless, shared.hudless, format(shared.hudless_format));
    else
        bind(dev, HUDLESS, g_hudless, placeholder(dev), format::r8g8b8a8_unorm);
}
} // namespace

void effects_register()
{
    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::register_event<reshade::addon_event::reshade_present>(on_present);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
}

void effects_unregister()
{
    reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
    reshade::unregister_event<reshade::addon_event::reshade_present>(on_present);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
}

bool effects_hudless_wanted()
{
    return g_hudless_used;
}
