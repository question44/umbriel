#pragma once

#include <cstddef>
#include <memory>
#include <vector>

struct wlr_surface;

namespace umbriel {

  class Server;

  // One glass shape of a toplevel, in surface-local logical pixels.
  struct GlassShape {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int radius = 0;
    bool operator==(const GlassShape&) const = default;
  };
  inline constexpr std::size_t kMaxGlassShapes = 16;

  // umbriel_glass_v1: clients name rounded rectangles of their toplevel that the backdrop effect draws as glass.
  // Shapes latch on wl_surface.commit, then the view's effects are re-applied.
  class GlassRegions {
  public:
    explicit GlassRegions(Server& server);
    ~GlassRegions();
    GlassRegions(const GlassRegions&) = delete;
    GlassRegions& operator=(const GlassRegions&) = delete;

    // The committed shapes of `surface`, or null when it has no glass object.
    static const std::vector<GlassShape>* shapes(wlr_surface* surface);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
