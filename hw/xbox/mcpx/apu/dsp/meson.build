dsp56300_emu_ffi = host_os == 'emscripten' ? not_found : dependency('dsp56300-emu-ffi', fallback: ['dsp56300', 'dsp56300_dep'])

subdir('interp')

# On emscripten the dsp56300 JIT backend (prebuilt Rust archive, no wasm
# target) is unavailable; dsp_jit.c depends on it, so compile only the
# pure-C backend and force use_dsp_jit off via dsp_jit.c stub below.
dsp_srcs = ['dsp.c', 'dsp_c.c', 'dsp_dma.c']
if host_os != 'emscripten'
  dsp_srcs += ['dsp_jit.c']
endif

libdsp = static_library(
  'dsp',
  files(dsp_srcs) + genh,
  dependencies: [dsp56300_emu_ffi],
)

dsp = declare_dependency(
  objects: [
    libdsp.extract_all_objects(recursive: false),
    libdsp_cpu_interp.extract_all_objects(recursive: false),
  ],
  dependencies: [dsp56300_emu_ffi],
)

mcpx_ss.add(dsp, files('gp_ep.c'))
