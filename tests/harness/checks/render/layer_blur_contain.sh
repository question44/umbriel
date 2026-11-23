#!/usr/bin/env bash
# `blur_contain` keeps a layer's live blur to what lies under the surface. A clear top bar sits right above a red
# window and over a green bottom-layer square: with the key, the bar's edge above the window shows no red, while the
# square under the bar still shows through (a live blur, not the cached wallpaper). Without the key the blur's reach
# (2^(passes + 1) * radius = 48 px) pulls the red window into the bar.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly SHOT="$UMBRIEL_RUNTIME_DIR/blur-contain.png"
readonly BAR_LOG="$UMBRIEL_RUNTIME_DIR/blur-contain-bar.log"
readonly TITLE=blur-contain-window
readonly BAR_HEIGHT=40

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = true
optimized = false
passes = 3
radius = 3
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[animation]
enabled = false

[[window_rule]]
match.title = "^blur-contain-window$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 300, y = 0, anchor = "top_left" }

[[layer_rule]]
match.namespace = "^umbriel-output-restore-regression$"
match.layer = "top"
blur = true
blur_contain = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Blue wallpaper, then the green 200x200 square in the bottom layer, pulled up under the bar (the bar's exclusive
# zone would otherwise place it below the bar).
readonly SQUARE_LOG="$UMBRIEL_RUNTIME_DIR/blur-contain-square.log"
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 > "$UMBRIEL_RUNTIME_DIR/blur-contain-wallpaper.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/blur-contain-wallpaper.log" ready 1
mkfifo "$UMBRIEL_RUNTIME_DIR/blur-contain-square.fifo"
env LAYER_CONTROL=1 "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 bottom-layer \
  < "$UMBRIEL_RUNTIME_DIR/blur-contain-square.fifo" > "$SQUARE_LOG" 2>&1 &
exec {square_fd}> "$UMBRIEL_RUNTIME_DIR/blur-contain-square.fifo"
await_lines "$SQUARE_LOG" ready 1

# The bar: a top exclusive zone, repainted fully clear so only its blur shows.
mkfifo "$UMBRIEL_RUNTIME_DIR/blur-contain-bar.fifo"
env LAYER_CONTROL=1 "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 "$BAR_HEIGHT" \
  < "$UMBRIEL_RUNTIME_DIR/blur-contain-bar.fifo" > "$BAR_LOG" 2>&1 &
exec {bar_fd}> "$UMBRIEL_RUNTIME_DIR/blur-contain-bar.fifo"
await_lines "$BAR_LOG" ready 1
echo "color bar 00000000" >&"$bar_fd"
await_lines "$BAR_LOG" done 1
echo "margins square -$BAR_HEIGHT 0 0 0" >&"$square_fd"
await_lines "$SQUARE_LOG" done 1

FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" "$TITLE" 400 300 > "$UMBRIEL_RUNTIME_DIR/$TITLE.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/$TITLE.log" mapped 1

read -r wy wx ww < <(
  "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.y) \(.x) \(.w)"'
)
if ((wy < BAR_HEIGHT || wy > BAR_HEIGHT + 2)); then
  echo "the window must end right at the bar's edge (y=$wy), not under it or away from it"
  exit 1
fi
readonly PROBE_X=$((wx + ww / 2)) PROBE_Y=$((BAR_HEIGHT - 2))

red_at_edge() {
  grim "$SHOT"
  "$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$PROBE_X" "$PROBE_Y" | cut -d' ' -f1
}

contained=$(red_at_edge)
read -r _ square_green _ < <("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel 60 20)
# The wallpaper is 0x5577AA: red 0x55 = 85 with nothing bleeding in.
if ((contained > 95)); then
  echo "contained blur took the window beside the bar: red=$contained at $PROBE_X,$PROBE_Y (window y=$wy)"
  exit 1
fi
if ((square_green < 200)); then
  echo "contained blur lost the bottom-layer square under the bar: green=$square_green"
  exit 1
fi

sed -i 's/^blur_contain = true$/blur_contain = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
echo "frames bar 2" >&"$bar_fd"
await_lines "$BAR_LOG" done 2
bled=$(red_at_edge)
if ((bled < contained + 20)); then
  echo "without blur_contain the window should bleed into the bar: red=$bled, contained=$contained"
  exit 1
fi

exec {bar_fd}>&- {square_fd}>&-
