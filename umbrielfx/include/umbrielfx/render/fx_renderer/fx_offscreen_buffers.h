#ifndef _FX_OFFSCREEN_BUFFERS_H
#define _FX_OFFSCREEN_BUFFERS_H

#include <stdbool.h>
#include <stdint.h>
#include <umbrielfx/render/effect.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>

// Scratch buffers sized to the current render target.
struct fx_target_buffers {
  // Blur swaps between the two effects buffers every time it scales the image
  // Buffer used for effects
  struct fx_framebuffer* effects_buffer;
  // Swap buffer used for effects
  struct fx_framebuffer* effects_buffer_swapped;
  // Copy of the target under an in-place effect, which the program samples
  struct fx_framebuffer* in_place_source;
  struct fx_framebuffer* animation_buffers[FX_ANIMATION_DEPTH];
  struct fx_framebuffer* animation_backdrop;
  // Unblurred copy of the target around a blur node with a backdrop effect
  struct fx_framebuffer* backdrop_source;
  // The surface's own alpha around a blur node with a backdrop effect
  struct fx_framebuffer* backdrop_shape;
};

// Light for liquid glass (wlr_scene_set_glass_light): the output (or its
// blurred wallpaper) reduced to a few dozen texels, read back asynchronously,
// and reduced on the CPU to the brightest area. All GL objects are allocated
// once per output size, never per frame.
#define FX_GLASS_LIGHT_LEVELS 2
struct fx_glass_light {
  unsigned textures[FX_GLASS_LIGHT_LEVELS];
  unsigned framebuffers[FX_GLASS_LIGHT_LEVELS];
  int widths[FX_GLASS_LIGHT_LEVELS], heights[FX_GLASS_LIGHT_LEVELS];
  int source_width, source_height; // output size the levels were made for
  bool float_levels; // RGBA16F levels (else RGBA8)
  bool failed;       // GL setup failed for this size: no light
  unsigned pbo;
  void* fence; // GLsync of the pending read, or NULL
  int64_t sampled_ms; // when the last read was started
  int64_t stepped_ms; // last smoothing step
  bool has_target;
  // x, y (output buffer texture coordinates), strength, r, g, b
  float target[6];
  float current[6];
  bool valid; // current holds a light the shaders see
  // Auto mode: a fullscreen window covers the output, so the screen is the source
  bool live;
  // Source used last frame (enum wlr_scene_glass_light_mode, never AUTO)
  int source;
  // Read the optimized blur at the end of the next frame (source went back to the wallpaper)
  bool want_wallpaper;
};

/**
 * Per-output renderer framebuffers. Each buffer is allocated on first use and
 * kept until it is no longer needed or the output goes away.
 */
struct fx_offscreen_buffers {
  struct wl_list link; // fx_renderer.offscreen_buffers
  struct wlr_addon addon;
  struct fx_renderer* renderer;
  // Allocator of the output the buffers belong to
  struct wlr_allocator* allocator;

  // Nearest-filtered spectrum, allocated only for an audio-reading shader.
  unsigned audio_texture;
  uint8_t audio_bands[FX_AUDIO_BANDS];

  struct fx_glass_light glass_light;

  // Linear-light composition target shared by the output swapchain
  struct fx_framebuffer* blend_buffer;
  bool blend_valid;
  uint64_t blend_generation;
  // sdr_tf the blend buffer was last composited with
  enum wlr_color_transfer_function blend_sdr_tf;
  // SDR view shared by capture of an HDR output, encoded with the pass sdr_tf
  struct fx_framebuffer* sdr_capture_buffer;
  uint64_t sdr_capture_generation;

  // Contains the blurred background for tiled windows
  struct fx_framebuffer* optimized_blur_buffer;
  // Whether the optimized blur buffers hold a completed render
  bool optimized_blur_valid;
  // sdr_tf the optimized blur buffers were rendered in linear light with, or 0
  // when they hold encoded values
  enum wlr_color_transfer_function optimized_blur_sdr_tf;
  // Contains the non-blurred background for tiled windows. Used for blurring
  // optimized surfaces with an alpha. Just as inefficient as the regular blur.
  struct fx_framebuffer* optimized_no_blur_buffer;
  // Contains the original pixels to draw over the areas where artifact are visible
  struct fx_framebuffer* blur_saved_pixels_buffer;
  // Sized to the output
  struct fx_target_buffers output;
  // Sized to a group capture and everything nested in it, so a group capture
  // never resizes the output set. Dropped after a pass that uses none.
  struct fx_target_buffers group;
};

void fx_offscreen_buffers_destroy(struct fx_offscreen_buffers* fbos);
void fx_offscreen_buffers_clear_group(struct fx_offscreen_buffers* fbos);
struct fx_offscreen_buffers* fx_offscreen_buffers_try_get(struct wlr_output* output);
void fx_offscreen_buffers_invalidate_blend(struct wlr_output* output);
void fx_renderer_clear_animation_buffers(struct wlr_output* output);
// Frees the glass light's GL objects; the renderer's context must be current.
void fx_glass_light_release(struct fx_glass_light* light);

#endif
