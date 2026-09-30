mcpx_ss.add(files(
	'apu.c',
	'debug.c',
	))
if host_os == 'emscripten'
  # monitor.c is SDL-audio based; wasm stubs keep the API but no stream
  mcpx_ss.add(files('monitor-wasm-stub.c'))
else
  mcpx_ss.add(sdl, files('monitor.c'))
endif

subdir('vp')
subdir('dsp')
