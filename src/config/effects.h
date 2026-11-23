#pragma once

#include "config/config_diag.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  class Section;

  enum class EffectKind : std::uint8_t { Animation, Border, Window, Screen, Cursor, Backdrop };
  enum class EffectSelectionPolicy : std::uint8_t { UnusedFirst, RoundRobin, Random };
  enum class EffectReferenceConstraint : std::uint8_t { PresetOnly, PresetOrPool, PoolRequired };

  std::optional<EffectSelectionPolicy> parseEffectSelectionPolicy(std::string_view text);
  std::string_view effectSelectionPolicyName(EffectSelectionPolicy policy);

  // Stable inner-to-outer composition order for effects sharing a target. Values equal the FX_SLOT_* indices.
  enum class AnimationEvent : unsigned {
    Window,
    Overlay,
    BorderEffect,
    Border,
    DimUnfocused,
    WindowsMove,
    Drag,
    WindowsIn,
    WindowsOut,
    Scratchpad,
    Layers,
    Workspaces,
    Overview
  };

  std::optional<EffectKind> parseEffectKind(std::string_view text);
  std::string_view effectKindName(EffectKind kind);

  // Keep source text in the resolved configuration. File edits then participate
  // in config equality, and render paths never perform filesystem I/O.
  struct ShaderSource {
    std::string code;
    std::filesystem::path file = {};
    bool operator==(const ShaderSource&) const = default;
  };

  struct [[nodiscard]] ShaderReadResult {
    std::optional<ShaderSource> source;
    // Includes missing files so creating one can trigger another config load.
    std::vector<std::filesystem::path> watchPaths;
  };

  inline constexpr std::size_t kShaderSourceLimit = 256 * 1024;

  // Reads the shader file path under `key`. Relative file paths belong to the
  // TOML value's source file, including when tables were merged.
  ShaderReadResult readShaderSource(Section& section, std::string_view key, std::vector<ConfigDiagnostic>& diagnostics);

  // The reserved selector value that disables a default per window or output.
  inline constexpr std::string_view kEffectOff = "off";

  struct BorderLight {
    int spread = 80;        // 1-256 logical px
    float intensity = 1.0F; // 0-4
    float threshold = 0.5F; // 0-1
    bool operator==(const BorderLight&) const = default;
  };

  struct EffectPreset {
    std::string name;
    EffectKind kind = EffectKind::Animation;
    // Empty code means the file was missing or unreadable: the preset exists so
    // references resolve, but it renders plainly.
    ShaderSource shader = {};
    bool palette = false;
    int padding = 0;          // border: 0-1024 logical px
    float speed = 1.0F;       // border: 0-10
    bool animated = true;     // border
    std::string overlay = {}; // NOLINT(readability-redundant-member-init) border: names a window preset
    std::optional<BorderLight> light = std::nullopt; // border
    int radius = 0;                                  // cursor: 0-4096, 0 = whole output
    // Shader constants, in declaration order: the preset's own `params`, then `[effects.params.<name>]` per key.
    // Each becomes `#define NAME value` ahead of the shader source.
    std::vector<std::pair<std::string, double>> params = {};
    bool inert() const { return shader.code.empty(); }
    bool operator==(const EffectPreset&) const = default;
  };

  struct EffectPool {
    std::string name;
    EffectKind kind = EffectKind::Border;
    std::vector<std::string> members;
    EffectSelectionPolicy selection = EffectSelectionPolicy::UnusedFirst;
    bool operator==(const EffectPool&) const = default;
  };

  struct Effects {
    std::vector<EffectPreset> presets;
    std::vector<EffectPool> pools;
    std::string border; // "" = off
    std::string window;
    std::string screen;
    // Replaces `screen` on an output while a client captures it whole. "" = none.
    std::string screenCast;
    std::string cursor;
    int maxFps = 0; // 0-240, 0 follows the refresh rate
    bool inCapture = false;
    bool borderUnfocused = false;
    // Light on liquid glass (umbriel_light): "off", "wallpaper" (the blurred
    // wallpaper's brightest area), "screen" (the composited output's) or "auto"
    // (the wallpaper, and the screen while a fullscreen window covers the output).
    std::string glassLight = "wallpaper";
    double glassLightRate = 15.0;     // screen samples per second, 1-60
    double glassLightSmoothing = 0.4; // seconds per e-fold of the easing, 0-5
    bool operator==(const Effects&) const = default;
  };

  const EffectPreset* findEffectPreset(const Effects& effects, std::string_view name);
  // The source a preset compiles: its params as `#define`s, then `#line 1` and the shader text.
  std::string effectShaderCode(const EffectPreset& preset);
  // Reads a `params` table into `target` (overriding names already there). Warns about entries that are not
  // GLSL-identifier = finite number, and about names the shader text never mentions.
  void readEffectParams(Section& params, const std::string& shaderCode, std::vector<std::pair<std::string, double>>& target);
  const EffectPool* findEffectPool(const Effects& effects, std::string_view name);
  // Empty and, when allowOff, "off" are valid; other references must satisfy kind and constraint.
  // Returns the diagnostic text on failure.
  std::optional<std::string> effectReferenceError(
      const Effects& effects, std::string_view name, EffectKind kind, bool allowOff,
      EffectReferenceConstraint constraint = EffectReferenceConstraint::PresetOnly
  );

} // namespace umbriel
