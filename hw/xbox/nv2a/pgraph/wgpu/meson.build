if host_os == 'emscripten'
webgpu = declare_dependency(
  compile_args: ['--use-port=emdawnwebgpu'],
  link_args: ['--use-port=emdawnwebgpu', '-sOFFSCREENCANVAS_SUPPORT=1'])
specific_ss.add([webgpu, files(
	'display.c',
	'renderer.c',
	)])
endif
