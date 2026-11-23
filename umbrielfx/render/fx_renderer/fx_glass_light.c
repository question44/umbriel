// Light for liquid glass: where the brightest area of an output is, so the
// glass rim can be lit from it (wlr_scene_set_glass_light).
//
// The source (the blurred wallpaper, or the composited output) is reduced on
// the GPU by two 8x8 box passes into small fixed-size targets, read into a
// pixel buffer behind a fence, and collected on a later frame, so the render
// never waits for the GPU. The CPU then finds the weighted centre of the
// brightest texels, their colour and how much they stand out, and eases the
// light towards that.

#include "glass_light_frag_src.h"
#include "render/egl.h"
#include "render/fx_renderer/fx_renderer.h"
#include "render/fx_renderer/shaders.h"
#include "umbrielfx/render/fx_renderer/fx_offscreen_buffers.h"
#include "umbrielfx/render/fx_renderer/fx_renderer.h"
#include "umbrielfx/render/pass.h"

#include <GLES3/gl3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

static const GLchar kGlassLightVert[] = "attribute vec2 pos;\n"
                                        "varying vec2 v_texcoord;\n"
                                        "void main() {\n"
                                        "  v_texcoord = pos;\n"
                                        "  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
                                        "}\n";

static bool ensure_program(struct fx_renderer* renderer) {
  if (renderer->glass_light_attempted) {
    return renderer->glass_light_program != 0;
  }
  renderer->glass_light_attempted = true;
  GLuint vert = compile_shader(GL_VERTEX_SHADER, kGlassLightVert);
  GLuint frag = vert != 0 ? compile_shader(GL_FRAGMENT_SHADER, glass_light_frag_src) : 0;
  if (frag == 0) {
    if (vert != 0) {
      glDeleteShader(vert);
    }
    return false;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glLinkProgram(prog);
  glDetachShader(prog, vert);
  glDetachShader(prog, frag);
  glDeleteShader(vert);
  glDeleteShader(frag);
  GLint ok = GL_FALSE;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (ok == GL_FALSE) {
    wlr_log(WLR_ERROR, "glass light: failed to link the reduction program");
    glDeleteProgram(prog);
    return false;
  }
  renderer->glass_light_program = prog;
  renderer->glass_light_pos = glGetAttribLocation(prog, "pos");
  renderer->glass_light_tex = glGetUniformLocation(prog, "tex");
  renderer->glass_light_texel = glGetUniformLocation(prog, "texel");
  return true;
}

static void release_levels(struct fx_glass_light* light) {
  for (int i = 0; i < FX_GLASS_LIGHT_LEVELS; i++) {
    if (light->framebuffers[i] != 0) {
      glDeleteFramebuffers(1, &light->framebuffers[i]);
    }
    if (light->textures[i] != 0) {
      glDeleteTextures(1, &light->textures[i]);
    }
    light->framebuffers[i] = 0;
    light->textures[i] = 0;
  }
  if (light->fence != NULL) {
    glDeleteSync((GLsync)light->fence);
    light->fence = NULL;
  }
  if (light->pbo != 0) {
    glDeleteBuffers(1, &light->pbo);
    light->pbo = 0;
  }
  light->source_width = light->source_height = 0;
  light->failed = false;
}

void fx_glass_light_release(struct fx_glass_light* light) {
  release_levels(light);
}

static bool make_level(struct fx_glass_light* light, int i, bool use_float) {
  glGenTextures(1, &light->textures[i]);
  glBindTexture(GL_TEXTURE_2D, light->textures[i]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (use_float) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, light->widths[i], light->heights[i], 0, GL_RGBA, GL_HALF_FLOAT, NULL);
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, light->widths[i], light->heights[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &light->framebuffers[i]);
  glBindFramebuffer(GL_FRAMEBUFFER, light->framebuffers[i]);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, light->textures[i], 0);
  return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// Levels for a source of this size: allocated only when the size changes.
static bool ensure_levels(struct fx_glass_light* light, int width, int height) {
  if (light->source_width == width && light->source_height == height) {
    return !light->failed;
  }
  release_levels(light);
  light->source_width = width;
  light->source_height = height;
  int w = width, h = height;
  for (int i = 0; i < FX_GLASS_LIGHT_LEVELS; i++) {
    w = (w + 7) / 8;
    h = (h + 7) / 8;
    light->widths[i] = w;
    light->heights[i] = h;
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    const bool use_float = attempt == 0;
    bool ok = true;
    for (int i = 0; i < FX_GLASS_LIGHT_LEVELS && ok; i++) {
      ok = make_level(light, i, use_float);
    }
    if (ok) {
      light->float_levels = use_float;
      break;
    }
    const int keep_w = light->source_width, keep_h = light->source_height;
    release_levels(light);
    light->source_width = keep_w;
    light->source_height = keep_h;
    if (!use_float) {
      light->failed = true;
    }
  }
  if (!light->failed) {
    const int last = FX_GLASS_LIGHT_LEVELS - 1;
    const size_t texel = light->float_levels ? 4 * sizeof(float) : 4;
    glGenBuffers(1, &light->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, light->pbo);
    glBufferData(
        GL_PIXEL_PACK_BUFFER, (GLsizeiptr)(light->widths[last] * light->heights[last] * texel), NULL, GL_STREAM_READ
    );
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }
  return !light->failed;
}

static void reduce(struct fx_renderer* renderer, GLuint source, int source_w, int source_h, GLuint target_fbo,
                   int target_w, int target_h) {
  glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
  glViewport(0, 0, target_w, target_h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, source);
  glUniform1i(renderer->glass_light_tex, 0);
  glUniform2f(renderer->glass_light_texel, 1.0F / (float)source_w, 1.0F / (float)source_h);
  static const GLfloat quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
  glVertexAttribPointer(renderer->glass_light_pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glEnableVertexAttribArray(renderer->glass_light_pos);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(renderer->glass_light_pos);
}

void fx_render_pass_sample_glass_light(
    struct fx_gles_render_pass* pass, struct fx_framebuffer* source, int64_t now_ms
) {
  struct fx_offscreen_buffers* fbos = pass->fx_offscreen_buffers;
  struct fx_renderer* renderer = pass->buffer->renderer;
  if (fbos == NULL || source == NULL || !renderer->is_gles3) {
    return;
  }
  struct fx_glass_light* light = &fbos->glass_light;
  if (light->fence != NULL) {
    return; // one read at a time; the next sample waits for it
  }
  const int width = source->buffer->width, height = source->buffer->height;
  if (!ensure_program(renderer) || !ensure_levels(light, width, height)) {
    fx_framebuffer_bind(pass->buffer);
    return;
  }
  struct wlr_texture* texture = fx_texture_from_buffer(&renderer->wlr_renderer, source->buffer);
  if (texture == NULL || fx_get_texture(texture)->target != GL_TEXTURE_2D) {
    if (texture != NULL) {
      wlr_texture_destroy(texture);
    }
    fx_framebuffer_bind(pass->buffer);
    return;
  }

  GLboolean blend = glIsEnabled(GL_BLEND);
  GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLboolean stencil = glIsEnabled(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glUseProgram(renderer->glass_light_program);
  reduce(renderer, fx_get_texture(texture)->tex, width, height, light->framebuffers[0], light->widths[0],
         light->heights[0]);
  for (int i = 1; i < FX_GLASS_LIGHT_LEVELS; i++) {
    reduce(renderer, light->textures[i - 1], light->widths[i - 1], light->heights[i - 1], light->framebuffers[i],
           light->widths[i], light->heights[i]);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  const int last = FX_GLASS_LIGHT_LEVELS - 1;
  glBindFramebuffer(GL_FRAMEBUFFER, light->framebuffers[last]);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, light->pbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, light->widths[last], light->heights[last], GL_RGBA,
               light->float_levels ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  light->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  light->sampled_ms = now_ms;

  wlr_texture_destroy(texture);
  if (blend) {
    glEnable(GL_BLEND);
  }
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  if (stencil) {
    glEnable(GL_STENCIL_TEST);
  }
  fx_framebuffer_bind(pass->buffer);
  glViewport(0, 0, pass->buffer->buffer->width, pass->buffer->buffer->height);
}

// The brightest area of the reduced image: weighted centre, colour, strength.
static void find_light(const struct fx_glass_light* light, const void* pixels, float out[6]) {
  const int last = FX_GLASS_LIGHT_LEVELS - 1;
  const int w = light->widths[last], h = light->heights[last], n = w * h;
  float* tone = malloc((size_t)n * sizeof(float));
  float(*rgb)[3] = malloc((size_t)n * sizeof(*rgb));
  out[0] = out[1] = 0.5F;
  out[2] = 0.0F;
  out[3] = out[4] = out[5] = 1.0F;
  if (tone == NULL || rgb == NULL) {
    free(tone);
    free(rgb);
    return;
  }
  double mean = 0.0;
  float peak = 0.0F;
  for (int i = 0; i < n; i++) {
    for (int c = 0; c < 3; c++) {
      rgb[i][c] = light->float_levels ? fmaxf(((const float*)pixels)[i * 4 + c], 0.0F)
                                      : ((const uint8_t*)pixels)[i * 4 + c] / 255.0F;
      if (!isfinite(rgb[i][c])) {
        rgb[i][c] = 0.0F;
      }
    }
    // Luminance, tone-mapped so an HDR highlight does not outweigh a whole
    // bright area: 1 nit-relative SDR white maps to 0.5.
    const float l = 0.2126F * rgb[i][0] + 0.7152F * rgb[i][1] + 0.0722F * rgb[i][2];
    tone[i] = l / (1.0F + l);
    mean += tone[i];
    peak = fmaxf(peak, tone[i]);
  }
  mean /= n;
  // Only the top half of the range between the mean and the peak counts.
  const float floor_tone = (float)mean + 0.5F * (peak - (float)mean);
  double sum = 0.0, sx = 0.0, sy = 0.0, sr = 0.0, sg = 0.0, sb = 0.0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const int i = y * w + x;
      const float d = tone[i] - floor_tone;
      if (d <= 0.0F) {
        continue;
      }
      const double wgt = (double)d * d * tone[i];
      sum += wgt;
      sx += wgt * ((x + 0.5) / w);
      sy += wgt * ((y + 0.5) / h);
      sr += wgt * rgb[i][0];
      sg += wgt * rgb[i][1];
      sb += wgt * rgb[i][2];
    }
  }
  if (sum > 1e-9) {
    out[0] = (float)(sx / sum);
    out[1] = (float)(sy / sum);
    // How much the bright area stands out from the average: a flat image has no light.
    out[2] = fminf(1.0F, fmaxf(0.0F, (peak - (float)mean) * 2.5F));
    const float r = (float)(sr / sum), g = (float)(sg / sum), b = (float)(sb / sum);
    const float m = fmaxf(fmaxf(r, g), fmaxf(b, 1e-4F));
    out[3] = r / m;
    out[4] = g / m;
    out[5] = b / m;
  }
  free(tone);
  free(rgb);
}

bool fx_glass_light_step(
    struct fx_renderer* renderer, struct fx_glass_light* light, int64_t now_ms, float smoothing, bool* busy
) {
  *busy = false;
  if (light->fence != NULL) {
    struct wlr_egl_context previous;
    if (!wlr_egl_make_current(renderer->egl, &previous)) {
      *busy = true;
      return false;
    }
    const GLenum status = glClientWaitSync((GLsync)light->fence, 0, 0);
    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
      glDeleteSync((GLsync)light->fence);
      light->fence = NULL;
      const int last = FX_GLASS_LIGHT_LEVELS - 1;
      const size_t bytes =
          (size_t)light->widths[last] * light->heights[last] * (light->float_levels ? 4 * sizeof(float) : 4);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, light->pbo);
      const void* pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
      if (pixels != NULL) {
        float found[6];
        find_light(light, pixels, found);
        // Deadband: small changes (a cursor, subtitles, a blinking caret) keep
        // the old target, so they cause no glass redraws.
        bool moved = !light->has_target;
        for (int i = 0; i < 6 && !moved; i++) {
          const float limit = i < 2 ? 0.03F : 0.05F;
          moved = fabsf(found[i] - light->target[i]) > limit;
        }
        if (moved) {
          memcpy(light->target, found, sizeof(found));
        }
        light->has_target = true;
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    } else if (status == GL_WAIT_FAILED) {
      glDeleteSync((GLsync)light->fence);
      light->fence = NULL;
    } else {
      *busy = true;
    }
    wlr_egl_restore_context(&previous);
  }
  if (!light->has_target) {
    return false;
  }
  if (!light->valid) {
    memcpy(light->current, light->target, sizeof(light->current));
    light->valid = true;
    light->stepped_ms = now_ms;
    return true;
  }
  // Ease towards the target. A long idle gap counts as one frame, so the
  // light still glides after a quiet spell instead of jumping.
  const int64_t dt_ms = now_ms - light->stepped_ms;
  light->stepped_ms = now_ms;
  const float dt = (float)(dt_ms < 0 ? 0 : dt_ms > 50 ? 16 : dt_ms) / 1000.0F;
  const float k = smoothing > 0.0F ? 1.0F - expf(-dt / smoothing) : 1.0F;
  bool changed = false;
  float remaining = 0.0F;
  for (int i = 0; i < 6; i++) {
    const float before = light->current[i];
    light->current[i] += (light->target[i] - light->current[i]) * k;
    if (fabsf(light->target[i] - light->current[i]) < 0.002F) {
      light->current[i] = light->target[i];
    }
    remaining = fmaxf(remaining, fabsf(light->target[i] - light->current[i]));
    changed = changed || light->current[i] != before;
  }
  *busy = *busy || remaining > 0.0F;
  return changed;
}

void fx_glass_light_uniforms(const struct fx_glass_light* light, float out_light[4], float out_color[3]) {
  if (light == NULL || !light->valid) {
    out_light[0] = out_light[1] = 0.5F;
    out_light[2] = out_light[3] = 0.0F;
    out_color[0] = out_color[1] = out_color[2] = 1.0F;
    return;
  }
  out_light[0] = light->current[0];
  out_light[1] = light->current[1];
  out_light[2] = light->current[2];
  out_light[3] = 1.0F;
  out_color[0] = light->current[3];
  out_color[1] = light->current[4];
  out_color[2] = light->current[5];
}
