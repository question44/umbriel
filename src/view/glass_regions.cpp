#include "view/glass_regions.h"

#include "server/server.h"
#include "view/view.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

extern "C" {
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "umbriel-glass-v1-server-protocol.h"
}

namespace umbriel {

  namespace {

    constexpr std::size_t kShapeInts = 6; // x, y, width, height, radius, flags

    struct Glass {
      wl_resource* resource = nullptr;
      wlr_surface* surface = nullptr;
      std::vector<GlassShape> pending;
      std::vector<GlassShape> current;
      bool dirty = false;
      wl_listener commit{};
      wl_listener surfaceDestroy{};
    };

    std::unordered_map<wlr_surface*, Glass*>& registry() {
      static std::unordered_map<wlr_surface*, Glass*> glasses;
      return glasses;
    }

    void refreshView(wlr_surface* surface) {
      if (View* view = View::fromSurface(surface)) {
        view->syncAnimationEffects();
      }
    }

    // Unhooks `glass` from its surface; the resource (if any) stays inert.
    void detach(Glass* glass) {
      if (glass->surface == nullptr) {
        return;
      }
      wl_list_remove(&glass->commit.link);
      wl_list_remove(&glass->surfaceDestroy.link);
      registry().erase(glass->surface);
      glass->surface = nullptr;
    }

    void handleCommit(wl_listener* listener, void*) {
      Glass* glass = wl_container_of(listener, glass, commit); // NOLINT(modernize-use-auto)
      if (!glass->dirty) {
        return;
      }
      glass->dirty = false;
      if (glass->pending == glass->current) {
        return;
      }
      glass->current = glass->pending;
      refreshView(glass->surface);
    }

    void handleSurfaceDestroy(wl_listener* listener, void*) {
      Glass* glass = wl_container_of(listener, glass, surfaceDestroy); // NOLINT(modernize-use-auto)
      detach(glass);
    }

    void destroyResource(wl_resource* resource) {
      auto* glass = static_cast<Glass*>(wl_resource_get_user_data(resource));
      if (glass == nullptr) {
        return;
      }
      wlr_surface* surface = glass->surface;
      const bool hadShapes = !glass->current.empty();
      detach(glass);
      delete glass;
      if (surface != nullptr && hadShapes) {
        refreshView(surface);
      }
    }

    void destroyRequest(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    void setShapes(wl_client*, wl_resource* resource, wl_array* array) {
      auto* glass = static_cast<Glass*>(wl_resource_get_user_data(resource));
      if (array->size % (kShapeInts * sizeof(int32_t)) != 0) {
        wl_resource_post_error(resource, UMBRIEL_GLASS_V1_ERROR_INVALID_SHAPES, "shape array of %zu bytes", array->size);
        return;
      }
      const std::size_t count = array->size / (kShapeInts * sizeof(int32_t));
      if (count > kMaxGlassShapes) {
        wl_resource_post_error(
            resource, UMBRIEL_GLASS_V1_ERROR_TOO_MANY_SHAPES, "%zu shapes, at most %zu", count, kMaxGlassShapes
        );
        return;
      }
      if (glass == nullptr) {
        return;
      }
      const auto* data = static_cast<const int32_t*>(array->data);
      glass->pending.clear();
      for (std::size_t i = 0; i < count; i++) {
        const int32_t* v = data + (i * kShapeInts);
        if (v[2] <= 0 || v[3] <= 0) {
          continue;
        }
        glass->pending.push_back({
            .x = v[0],
            .y = v[1],
            .width = v[2],
            .height = v[3],
            .radius = std::clamp(v[4], 0, std::min(v[2], v[3]) / 2),
        });
      }
      glass->dirty = true;
    }

    const struct umbriel_glass_v1_interface kGlass{
        .destroy = destroyRequest,
        .set_shapes = setShapes,
    };

    void getGlass(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surfaceResource) {
      wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
      if (registry().contains(surface)) {
        wl_resource_post_error(manager, UMBRIEL_GLASS_MANAGER_V1_ERROR_ALREADY_EXISTS, "surface already has glass");
        return;
      }
      wl_resource* resource = wl_resource_create(client, &umbriel_glass_v1_interface, wl_resource_get_version(manager), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      auto* glass = new Glass();
      glass->resource = resource;
      glass->surface = surface;
      glass->commit.notify = handleCommit;
      wl_signal_add(&surface->events.commit, &glass->commit);
      glass->surfaceDestroy.notify = handleSurfaceDestroy;
      wl_signal_add(&surface->events.destroy, &glass->surfaceDestroy);
      registry()[surface] = glass;
      wl_resource_set_implementation(resource, &kGlass, glass, destroyResource);
    }

    const struct umbriel_glass_manager_v1_interface kManager{
        .destroy = destroyRequest,
        .get_glass = getGlass,
    };

  } // namespace

  struct GlassRegions::Impl {
    wl_global* global = nullptr;
  };

  GlassRegions::GlassRegions(Server& server) : m_impl(std::make_unique<Impl>()) {
    m_impl->global = wl_global_create(
        server.display(), &umbriel_glass_manager_v1_interface, 1, nullptr,
        [](wl_client* client, void*, uint32_t version, uint32_t id) {
          wl_resource* resource = wl_resource_create(client, &umbriel_glass_manager_v1_interface, version, id);
          if (resource == nullptr) {
            wl_client_post_no_memory(client);
            return;
          }
          wl_resource_set_implementation(resource, &kManager, nullptr, nullptr);
        }
    );
    if (m_impl->global == nullptr) {
      wlr_log(WLR_ERROR, "failed to create the umbriel_glass_manager_v1 global");
    }
  }

  GlassRegions::~GlassRegions() {
    if (m_impl->global != nullptr) {
      wl_global_destroy(m_impl->global);
    }
    // Leave resources alive (their clients go with the display) but stop them touching surfaces.
    auto& glasses = registry();
    while (!glasses.empty()) {
      detach(glasses.begin()->second);
    }
  }

  const std::vector<GlassShape>* GlassRegions::shapes(wlr_surface* surface) {
    const auto& glasses = registry();
    const auto it = glasses.find(surface);
    return it == glasses.end() ? nullptr : &it->second->current;
  }

} // namespace umbriel
