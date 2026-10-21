#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

// struct wlr_vk_frag_texture_pcr_data
layout(push_constant, row_major) uniform UBO {
	layout(offset = 80) mat4 matrix;
	float alpha;
	float luminance_multiplier;
	bool swap_xy;
	bool flip_x;
	bool flip_y;
	float radius_top;
	float radius_bottom;
	float pad;
	vec4 box;
} data;

layout (constant_id = 0) const int TEXTURE_TRANSFORM = 0;

// Matches enum wlr_vk_texture_transform
#define TEXTURE_TRANSFORM_IDENTITY 0
#define TEXTURE_TRANSFORM_SRGB 1
#define TEXTURE_TRANSFORM_ST2084_PQ 2
#define TEXTURE_TRANSFORM_GAMMA22 3
#define TEXTURE_TRANSFORM_BT1886 4

float srgb_channel_to_linear(float x) {
	return mix(x / 12.92,
		pow((x + 0.055) / 1.055, 2.4),
		x > 0.04045);
}

vec3 srgb_color_to_linear(vec3 color) {
	return vec3(
		srgb_channel_to_linear(color.r),
		srgb_channel_to_linear(color.g),
		srgb_channel_to_linear(color.b)
	);
}

vec3 pq_color_to_linear(vec3 color) {
	float inv_m1 = 1 / 0.1593017578125;
	float inv_m2 = 1 / 78.84375;
	float c1 = 0.8359375;
	float c2 = 18.8515625;
	float c3 = 18.6875;
	vec3 num = max(pow(color, vec3(inv_m2)) - c1, 0);
	vec3 denom = c2 - c3 * pow(color, vec3(inv_m2));
	return pow(num / denom, vec3(inv_m1));
}

vec3 bt1886_color_to_linear(vec3 color) {
	float Lmin = 0.01;
	float Lmax = 100.0;
	float lb = pow(Lmin, 1.0 / 2.4);
	float lw = pow(Lmax, 1.0 / 2.4);
	float a  = pow(lw - lb, 2.4);
	float b  = lb / (lw - lb);
	vec3 L = a * pow(color + vec3(b), vec3(2.4));
	return (L - Lmin) / (Lmax - Lmin);
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

// Ported from niri's rounding_alpha.frag, with an extra `expand` term.
//
// `corner_radius` is the radius of the corner arc that is cut away, with its
// center at `(corner_radius, corner_radius)` from the corner. `expand` shifts
// the distance threshold outward without moving that center. Changing the
// radius instead would move the center too, which is why the window content is
// grown with `expand`, not by adding to the radius.
float niri_rounding_alpha(vec2 coords, vec2 size, vec4 corner_radius, float expand) {
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

    float dist = distance(coords, center) - expand;

    // Manual smoothstep() between radius - half_px and radius + half_px
    // to avoid a division in clamp().
    float t = clamp((dist - radius) * niri_scale + 0.5, 0.0, 1.0);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

void main() {
	vec4 in_color = textureLod(tex, uv, 0);

	// Convert from pre-multiplied alpha to straight alpha
	float alpha = in_color.a;
	vec3 rgb;
	if (alpha == 0) {
		rgb = vec3(0);
	} else {
		rgb = in_color.rgb / alpha;
	}

	if (TEXTURE_TRANSFORM != TEXTURE_TRANSFORM_IDENTITY) {
		rgb = max(rgb, vec3(0));
	}
	if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_SRGB) {
		rgb = srgb_color_to_linear(rgb);
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_ST2084_PQ) {
		rgb = pq_color_to_linear(rgb);
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_GAMMA22) {
		rgb = pow(rgb, vec3(2.2));
	} else if (TEXTURE_TRANSFORM == TEXTURE_TRANSFORM_BT1886) {
		rgb = bt1886_color_to_linear(rgb);
	}

	rgb *= data.luminance_multiplier;

	rgb = mat3(data.matrix) * rgb;

	// Back to pre-multiplied alpha
	out_color = vec4(rgb * alpha, alpha);

	vec4 geo = niri_geo(gl_FragCoord.xy);
	vec2 rel = geo.xy;
	vec2 size = geo.zw;

	if (rel.x < 0.0 || rel.x > size.x || rel.y < 0.0 || rel.y > size.y) {
		out_color = vec4(0.0);
		return;
	}

	vec4 corner_radius = vec4(data.radius_top, data.radius_top, data.radius_bottom, data.radius_bottom);
	// The decoration border is composited on top of the window content and
	// cuts its inner corner at the same radius. Without help, the two
	// antialiased edges meet at the same distance with no opaque content behind
	// the border, so the background (and the shadow) shows through as a
	// sub-pixel seam at the corners -- a dark/coloured fringe on light
	// backgrounds. Grow the content's corner solid region by one physical pixel
	// so it always tucks under the border's inner antialiasing.
	float opacity = niri_rounding_alpha(rel, size, corner_radius, 1.0);

	out_color *= opacity * data.alpha;
}
