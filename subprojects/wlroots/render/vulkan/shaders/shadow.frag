#version 450

layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform UBO {
	bool enabled;
	float blur;
	float radius_top;
	float radius_bottom;
	bool swap_xy;
	bool flip_x;
	bool flip_y;
	float pad;
	vec4 box;
	vec4 color;
} data;

// https://gitlab.gnome.org/GNOME/gtk/-/blob/gtk-4-16/gsk/gpu/shaders/gskgpuboxshadow.glsl
#define PI 3.14159265358979323846
#define SQRT1_2 0.70710678118654752440

/* A standard gaussian function, used for weighting samples */
float gauss(float x, float sigma) {
  float sigma_2 = sigma * sigma;
  return 1.0 / sqrt(2.0 * PI * sigma_2) * exp(-(x * x) / (2.0 * sigma_2));
}

/* This approximates the error function, needed for the gaussian integral */
vec2 erf(vec2 x) {
  vec2 s = sign(x), a = abs(x);
  x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
  x *= x;
  return s - s / (x * x);
}

float erf_range(vec2 x, float sigma) {
  vec2 from_to = 0.5 - 0.5 * erf (x / (sigma * SQRT1_2));
  return from_to.y - from_to.x;
}

float ellipse_x(vec2 ellipse, float y) {
  float y_scaled = y / ellipse.y;
  return ellipse.x * sqrt(1.0 - y_scaled * y_scaled);
}

float blur_rect(vec2 rel, vec2 siz, float sigma) {
  return erf_range(vec2(-rel.x, siz.x - rel.x), sigma) * erf_range(vec2(-rel.y, siz.y - rel.y), sigma);
}

float blur_corner(vec2 p, vec2 r, float sigma) {
  if (min (r.x, r.y) <= 0.0)
    return 0.0;

  p /= sigma;
  r /= sigma;

  if (min (p.x, p.y) <= -2.95 ||
      max (p.x - r.x, p.y - r.y) >= 2.95)
    return 0.0;

  float result = 0.0;
  float start = max(p.y - 3.0, 0.0);
  float end = min(p.y + 3.0, r.y);
  float step = (end - start) / 7.0;
  float y = start;
  for (int i = 0; i < 8; i++) {
      float x = r.x - ellipse_x(r, r.y - y);
      result -= gauss(p.y - y, 1.0) * erf_range(vec2 (- p.x, x - p.x), 1.0);
      y += step;
    }
  return step * result;
}

// Evaluated in the unified geometry space, so the flip/swap transform is not
// applied a second time here.
float blur_rounded_rect (vec4 r, vec2 p, float sigma) {
    vec2 rel = p - r.xy;
    vec2 siz = r.zw;
  float result = blur_rect(rel, siz, sigma);
  result -= blur_corner(rel, vec2(data.radius_top, data.radius_top), sigma);
  result -= blur_corner(vec2(siz.x - rel.x, rel.y), vec2(data.radius_top, data.radius_top), sigma);
  result -= blur_corner(siz - rel, vec2(data.radius_bottom, data.radius_bottom), sigma);
  result -= blur_corner(vec2(rel.x, siz.y - rel.y), vec2(data.radius_bottom, data.radius_bottom), sigma);

  return result;
}

// ---------------------------------------------------------------------------
// Rounded corners, ported from niri (niri/src/render_helpers/shaders).
// ---------------------------------------------------------------------------

// niri's `niri_scale` is the output scale, because niri expresses geometry and
// radii in logical pixels. wlroots passes both in physical pixels, so the
// equivalent antialiasing scale is 1.0 (a one-physical-pixel transition).
const float niri_scale = 1.0;

// Unified geometry space.
//
// niri maps input coordinates into a window-local "geometry space" with an
// `input_to_geo` matrix and always evaluates the rounded-corner mask there.
// wlroots exposes the same information via `box` and the output-transform
// flags, so this builds the equivalent mapping. It returns
// (coords.xy, size.xy): `coords` is relative to the top-left of the
// untransformed geometry and `size` is that geometry's size, both in physical
// pixels.
vec4 niri_geo(vec2 frag) {
    vec2 rel = frag - data.box.xy;
    if (data.flip_x) {
        rel.x = data.box.z - rel.x;
    }
    if (data.flip_y) {
        rel.y = data.box.w - rel.y;
    }
    vec2 size;
    if (data.swap_xy) {
        rel = rel.yx;
        size = vec2(data.box.w, data.box.z);
    } else {
        size = vec2(data.box.z, data.box.w);
    }
    return vec4(rel, size);
}

// Ported verbatim from niri's rounding_alpha.frag.
float niri_rounding_alpha(vec2 coords, vec2 size, vec4 corner_radius) {
    vec2 center;
    float radius;

    if (coords.x < corner_radius.x && coords.y < corner_radius.x) {
        radius = corner_radius.x;
        center = vec2(radius, radius);
    } else if (size.x - corner_radius.y < coords.x && coords.y < corner_radius.y) {
        radius = corner_radius.y;
        center = vec2(size.x - radius, radius);
    } else if (size.x - corner_radius.z < coords.x && size.y - corner_radius.z < coords.y) {
        radius = corner_radius.z;
        center = vec2(size.x - radius, size.y - radius);
    } else if (coords.x < corner_radius.w && size.y - corner_radius.w < coords.y) {
        radius = corner_radius.w;
        center = vec2(radius, size.y - radius);
    } else {
        return 1.0;
    }

    float dist = distance(coords, center);

    // Manual smoothstep() between radius - half_px and radius + half_px
    // to avoid a division in clamp().
    float t = clamp((dist - radius) * niri_scale + 0.5, 0.0, 1.0);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

void main() {
	if (!data.enabled) {
		discard;
	}

    vec4 geo = niri_geo(gl_FragCoord.xy);
    vec2 rel = geo.xy;
    vec2 size = geo.zw;

    float opacity;
    if (data.blur > 0.0) {
        // Blurred shadow: keep the GTK-style approximation, evaluated in the
        // unified geometry space.
        vec4 b = vec4(data.blur, data.blur, size.x - 2.0 * data.blur, size.y - 2.0 * data.blur);
        opacity = blur_rounded_rect(b, rel, data.blur);
    } else {
        // Sharp shadow: the same rounded-rectangle mask as niri's shadow.
        vec4 corner_radius = vec4(data.radius_top, data.radius_top, data.radius_bottom, data.radius_bottom);
        opacity = niri_rounding_alpha(rel, size, corner_radius);
    }

    out_color = data.color * opacity;
}
