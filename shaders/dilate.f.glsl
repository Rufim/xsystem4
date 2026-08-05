/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

uniform sampler2D tex;
uniform float threshold;
uniform vec4 color;  // .a is the discard threshold

in vec2 tex_coord;
out vec4 frag_color;

void main() {
	// Morphological dilation by `threshold` pixels done in the DISTANCE domain
	// (max-plus), not as a max of neighbour alphas.
	//
	// The glyph texture is a coverage field, and for an antialiased edge coverage
	// encodes the signed distance from the pixel centre to the outline:
	// a = 0.5 - d. So a sample `a` at offset k implies the shape is at distance
	// |k| + 0.5 - a from us; growing the shape by r moves that outline inward by
	// r, and the new coverage is
	//     a' = clamp(max over k of [a(p+k) - |k|] + r)
	// taken over samples that actually carry distance information (a > 0; a == 0
	// only means "at least half a pixel away", so including it would paint an
	// r-wide halo over empty space).
	//
	// This SATURATES, and that is the whole point: a 1px partially-covered stroke
	// dilated by r = 0.5 becomes a solid 2px stroke. A max of alphas cannot do
	// that — the fractional part of r only down-weighted the outermost ring, so
	// r < 1 was a no-op (太さ = 0.5 in Tsumamigui 3's BACK LOG drew hairlines),
	// and r = 1 widened the stroke to 3px while leaving it soft. Measured against
	// the original, the reference glyph is both NARROWER and DENSER than that:
	// 2px strokes at full coverage (FINDINGS §5j).
	int cutoff = int(ceil(threshold)) + 1;
	vec2 tex_size = vec2(textureSize(tex, 0).xy);

	// sentinel: no sample carries coverage ⇒ stays empty (no halo)
	float best = -1024.0;

	for (int x = -cutoff; x <= cutoff; x++) {
		for (int y = -cutoff; y <= cutoff; y++) {
			float d = length(vec2(x, y));
			// past this the term cannot be positive for any coverage
			if (d > threshold + 1.0)
				continue;
			float a = texture(tex, tex_coord + vec2(x, y) / tex_size).r;
			if (a <= 0.0)
				continue;
			best = max(best, a - d);
		}
	}
	float a_out = clamp(best + threshold, 0.0, 1.0);

	if (a_out < color.a)
		discard;
	frag_color = vec4(color.rgb, a_out);
}
