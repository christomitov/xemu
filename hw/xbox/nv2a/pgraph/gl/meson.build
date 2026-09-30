if host_os != 'emscripten'
  # GL renderer needs SDL + gloffscreen; wasm uses the null renderer.
  specific_ss.add([sdl, gloffscreen, files(
	'blit.c',
	'debug.c',
	'display.c',
	'draw.c',
	'gpuprops.c',
	'renderer.c',
	'reports.c',
	'shaders.c',
	'surface.c',
	'texture.c',
	'vertex.c',
	)])
endif
