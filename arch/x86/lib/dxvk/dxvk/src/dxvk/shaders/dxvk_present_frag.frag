#version 450

layout(binding = 0) uniform sampler2D s_image;

layout(location = 0) out vec4 o_color;

layout(push_constant)
uniform present_info_t {
  ivec2 src_offset;
  ivec2 dst_offset;
};

void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy) + src_offset - dst_offset;
  o_color = texelFetch(s_image, coord, 0);
  
  /* OsitoK/Venus-on-macOS rejects present pipelines that declare Sampled1D,
   * even when the gamma path is disabled by specialization constants. Keep
   * the copy-present shader strictly 2D until 1D gamma LUTs are supported. */
}
