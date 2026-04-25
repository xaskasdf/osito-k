/*
 * venus_cmd_present_software_raster.c — CPU-fallback triangle rasterizer.
 *
 * W3b.6 — invoked from venus_cmd_QueuePresentKHR.c when the swapchain
 * image is SHM-backed and a recorded CmdDraw bound a vertex buffer with
 * vertexCount >= 3. Paints one triangle in solid color via barycentric
 * scan-conversion over the AABB. No texturing, no depth, no blending.
 *
 * NDC mapping:
 *   x in [-1,+1] -> screen x in [0, fb_w-1]
 *   y in [-1,+1] -> screen y in [fb_h-1, 0]   (flip — Vulkan NDC has +Y down,
 *                                              but this matches the natural
 *                                              "smaller y = higher row" feel
 *                                              the test app expects.)
 *
 * Pure C, no libc. The fb is 32bpp BGRA (the format we asked for in
 * SYS_SHM_MKSURFACE).
 */
#include <stdint.h>

/* Read a single float at (verts + index*stride_floats + comp). */
static inline float vsr_get(const float *verts, uint32_t index,
                            uint32_t stride_floats, uint32_t comp) {
    return verts[index * stride_floats + comp];
}

static inline int32_t vsr_imax(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t vsr_imin(int32_t a, int32_t b) { return a < b ? a : b; }

/* Edge function — twice the signed area of triangle (a,b,p). */
static inline float vsr_edge(float ax, float ay, float bx, float by,
                             float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void vsr_paint_triangle(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                        const float *verts, uint32_t stride_floats,
                        uint32_t bgra_color) {
    if (!fb || !verts || fb_w == 0 || fb_h == 0) return;
    if (stride_floats < 3u) stride_floats = 3u;

    /* Pull NDC (x, y) for each of the 3 vertices, ignore z. */
    float x0 = vsr_get(verts, 0, stride_floats, 0);
    float y0 = vsr_get(verts, 0, stride_floats, 1);
    float x1 = vsr_get(verts, 1, stride_floats, 0);
    float y1 = vsr_get(verts, 1, stride_floats, 1);
    float x2 = vsr_get(verts, 2, stride_floats, 0);
    float y2 = vsr_get(verts, 2, stride_floats, 1);

    /* Map NDC -> screen. Flip Y so that NDC -1 is at the bottom of the
     * screen and +1 is at the top. That matches the hello-triangle vertex
     * layout (apex at (0, -0.7) appears at the top). */
    float w = (float)(int32_t)fb_w;
    float h = (float)(int32_t)fb_h;
    float sx0 = (x0 * 0.5f + 0.5f) * (w - 1.0f);
    float sy0 = (1.0f - (y0 * 0.5f + 0.5f)) * (h - 1.0f);
    float sx1 = (x1 * 0.5f + 0.5f) * (w - 1.0f);
    float sy1 = (1.0f - (y1 * 0.5f + 0.5f)) * (h - 1.0f);
    float sx2 = (x2 * 0.5f + 0.5f) * (w - 1.0f);
    float sy2 = (1.0f - (y2 * 0.5f + 0.5f)) * (h - 1.0f);

    /* AABB in screen space, clamped to the framebuffer. */
    int32_t fb_w_i = (int32_t)fb_w;
    int32_t fb_h_i = (int32_t)fb_h;
    int32_t min_x = (int32_t)sx0; if ((int32_t)sx1 < min_x) min_x = (int32_t)sx1;
    if ((int32_t)sx2 < min_x) min_x = (int32_t)sx2;
    int32_t max_x = (int32_t)sx0; if ((int32_t)sx1 > max_x) max_x = (int32_t)sx1;
    if ((int32_t)sx2 > max_x) max_x = (int32_t)sx2;
    int32_t min_y = (int32_t)sy0; if ((int32_t)sy1 < min_y) min_y = (int32_t)sy1;
    if ((int32_t)sy2 < min_y) min_y = (int32_t)sy2;
    int32_t max_y = (int32_t)sy0; if ((int32_t)sy1 > max_y) max_y = (int32_t)sy1;
    if ((int32_t)sy2 > max_y) max_y = (int32_t)sy2;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x > fb_w_i - 1) max_x = fb_w_i - 1;
    if (max_y > fb_h_i - 1) max_y = fb_h_i - 1;
    if (min_x > max_x || min_y > max_y) return;

    /* Triangle area sign — CCW vs CW. We accept both (no backface cull). */
    float area = vsr_edge(sx0, sy0, sx1, sy1, sx2, sy2);
    if (area == 0.0f) return;

    for (int32_t py = min_y; py <= max_y; py++) {
        uint32_t row_off = (uint32_t)py * fb_w;
        for (int32_t px = min_x; px <= max_x; px++) {
            float fx = (float)px + 0.5f;
            float fy = (float)py + 0.5f;
            float w0 = vsr_edge(sx1, sy1, sx2, sy2, fx, fy);
            float w1 = vsr_edge(sx2, sy2, sx0, sy0, fx, fy);
            float w2 = vsr_edge(sx0, sy0, sx1, sy1, fx, fy);
            /* Inside-test that handles both winding orders. */
            int inside;
            if (area > 0.0f) inside = (w0 >= 0.0f) && (w1 >= 0.0f) && (w2 >= 0.0f);
            else             inside = (w0 <= 0.0f) && (w1 <= 0.0f) && (w2 <= 0.0f);
            if (inside)
                fb[row_off + (uint32_t)px] = bgra_color;
        }
    }
}
