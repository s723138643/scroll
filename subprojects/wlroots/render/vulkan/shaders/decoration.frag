#version 450

layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform UBO {
	bool border;
	bool title_bar;
	bool dim;
	float border_width;
	float border_radius;
	float title_bar_height;
	float title_bar_border_radius;
	bool swap_xy;
	bool flip_x;
	bool flip_y;
	float pad1;
	float pad2;
	vec4 box;
	vec4 border_top;
	vec4 border_bottom;
	vec4 border_left;
	vec4 border_right;
	vec4 title_bar_color;
	vec4 dim_color;
	vec4 pad3;
} data;

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
    vec4 geo = niri_geo(gl_FragCoord.xy);
    vec2 rel = geo.xy;
    vec2 size = geo.zw;
    float width = size.x;
    float height = size.y;

    float title_height = 0.0;

    if (data.title_bar) {
        title_height = data.title_bar_height;

        // The title bar is a rounded rectangle with only its top corners cut.
        if (rel.y <= data.title_bar_height) {
            vec4 corner_radius = vec4(
                data.title_bar_border_radius,
                data.title_bar_border_radius,
                0.0,
                0.0
            );
            float opacity = niri_rounding_alpha(rel, vec2(width, data.title_bar_height), corner_radius);
            out_color = data.title_bar_color * opacity;
            return;
        }
    }

    if (data.border) {
        rel.y -= title_height;
        height -= title_height;

        float bw = data.border_width;

        // wlroots' border_radius is the radius of the window content, which is
        // inset by the border width, so the border reaches out to
        // border_radius + bw. A zero radius keeps square corners, matching the
        // previous wlroots behavior.
        float top_radius = data.title_bar ? 0.0 : data.border_radius;
        float bottom_radius = data.border_radius;
        float top_outer = top_radius > 0.0 ? top_radius + bw : 0.0;
        float bottom_outer = bottom_radius > 0.0 ? bottom_radius + bw : 0.0;
        vec4 outer_radius = vec4(top_outer, top_outer, bottom_outer, bottom_outer);

        vec2 border_size = vec2(width, height);
        float outer_alpha = niri_rounding_alpha(rel, border_size, outer_radius);

        vec2 inner_size = border_size - vec2(bw * 2.0);
        vec4 inner_radius = max(outer_radius - vec4(bw), 0.0);
        vec2 inner_coords = rel - vec2(bw);
        float inner_alpha = 0.0;
        if (inner_coords.x >= 0.0 && inner_coords.x <= inner_size.x &&
                inner_coords.y >= 0.0 && inner_coords.y <= inner_size.y) {
            inner_alpha = niri_rounding_alpha(inner_coords, inner_size, inner_radius);
        }

        float ring_alpha = outer_alpha * (1.0 - inner_alpha);

        if (ring_alpha > 0.0) {
            // wlroots has four border colors instead of niri's gradient. Pick
            // the nearest edge; at the corners this produces the same diagonal
            // split the old circumference() code used.
            float d_left = rel.x;
            float d_right = width - rel.x;
            float d_top = rel.y;
            float d_bottom = height - rel.y;
            vec4 border_color;
            if (min(d_left, d_right) < min(d_top, d_bottom)) {
                border_color = d_left < d_right ? data.border_left : data.border_right;
            } else {
                border_color = d_top < d_bottom ? data.border_top : data.border_bottom;
            }

            vec4 color = border_color * ring_alpha;
            // Only the content side of the ring is dimmed, as before.
            if (data.dim && inner_alpha > 0.0) {
                color = mix(data.dim_color, color, color.a);
            }
            out_color = color;
            return;
        }

        // Outside the outer rounded rectangle.
        if (outer_alpha <= 0.0) {
            out_color = vec4(0.0);
            return;
        }
    }

    if (data.dim) {
        out_color = data.dim_color;
    } else {
        discard;
    }
}
