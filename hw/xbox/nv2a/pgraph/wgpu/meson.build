if host_os == 'emscripten'
# GLSL -> SPIR-V -> WGSL translator (glslang + Tint), prebuilt for wasm in
# /root/src/xemu-wasm/wgsl-toolchain (see its LINK.txt)
wgsl_tc = '/root/src/xemu-wasm/wgsl-toolchain/dist'
webgpu = declare_dependency(
  compile_args: ['--use-port=emdawnwebgpu', '-I' + wgsl_tc + '/include'],
  link_args: ['--use-port=emdawnwebgpu', '-sOFFSCREENCANVAS_SUPPORT=1',
              '-L' + wgsl_tc + '/lib', '-lxemu_wgsl_all', '-lc++', '-lc++abi',
              # translation never suspends: keep it out of Asyncify (-4 MB)
              '-sASYNCIFY_REMOVE=["tint::*","glslang::*","spvtools::*","spv::*","absl::*","xemu_glsl_to_*","xemu_spirv_to_wgsl","xemu_wgsl_*"]'])
specific_ss.add([webgpu, files(
	'blit.c',
	'buffer.c',
	'command.c',
	'display.c',
	'draw.c',
	'glsl.c',
	'renderer.c',
	'reports.c',
	'shaders.c',
	'surface-compute.c',
	'surface-reshape.c',
	'surface.c',
	'texture.c',
	'vertex.c',
	)])
endif
