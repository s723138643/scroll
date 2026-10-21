#extension GL_OES_EGL_image_external : require

precision highp float;

varying vec2 v_texcoord;
uniform samplerExternalOES texture0;
uniform float alpha;
uniform vec4 box;
uniform bool swap_xy;
uniform bool flip_x;
uniform bool flip_y;
uniform float radius_top;
uniform float radius_bottom;

// ---------------------------------------------------------------------------
// Rounded corners, ported from niri (niri/src/render_helpers/shaders).
//
// The algorithm lives in niri's rounding_alpha.frag. wlroots has no per-corner
// radius uniform, so the four radii are built from the existing
// radius_top / radius_bottom pair.
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
    vec2 rel = frag - box.xy;
    if (flip_x) {
        rel.x = box.z - rel.x;
    }
    if (flip_y) {
        rel.y = box.w - rel.y;
    }
    vec2 size;
    if (swap_xy) {
        rel = rel.yx;
        size = vec2(box.w, box.z);
    } else {
        size = vec2(box.z, box.w);
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
    vec4 geo = niri_geo(gl_FragCoord.xy);
    vec2 rel = geo.xy;
    vec2 size = geo.zw;

    if (rel.x < 0.0 || rel.x > size.x || rel.y < 0.0 || rel.y > size.y) {
        gl_FragColor = vec4(0.0);
        return;
    }

    vec4 corner_radius = vec4(radius_top, radius_top, radius_bottom, radius_bottom);
    // The decoration border is composited on top of the window content and
    // cuts its inner corner at the same radius. Without help, the two
    // antialiased edges meet at the same distance with no opaque content behind
    // the border, so the background (and the shadow) shows through as a
    // sub-pixel seam at the corners -- a dark/coloured fringe on light
    // backgrounds. Grow the content's corner solid region by one physical pixel
    // so it always tucks under the border's inner antialiasing.
    float opacity = niri_rounding_alpha(rel, size, corner_radius, 1.0);

    gl_FragColor = texture2D(texture0, v_texcoord) * opacity * alpha;
}
