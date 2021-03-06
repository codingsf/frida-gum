gumpp_headers = [
  'gumpp.hpp',
]

gumpp_sources = [
  'backtracer.cpp',
  'interceptor.cpp',
  'invocationlistener.cpp',
  'profiler.cpp',
  'returnaddress.cpp',
  'runtime.cpp',
  'sampler.cpp',
  'sanitychecker.cpp',
  'symbolutil.cpp',
]

extra_link_args = []
extra_link_depends = []
if host_os_family == 'darwin'
  symlist = 'gumpp.symbols'
  extra_link_args += ['-Wl,-exported_symbols_list,' + join_paths(meson.current_source_dir(), symlist)]
  extra_link_depends += [symlist]
elif host_os_family != 'windows'
  symscript = 'gumpp.version'
  extra_link_args += ['-Wl,--version-script,' + join_paths(meson.current_source_dir(), symscript)]
  extra_link_depends += [symscript]
endif

install_headers(gumpp_headers, subdir: install_header_basedir + '/gumpp')

gumpp = shared_library('frida-gumpp-' + api_version, gumpp_sources,
  link_args: extra_link_args,
  link_depends: extra_link_depends,
  vs_module_defs: 'gumpp.def',
  dependencies: [gum_dep, gum_heap_dep, gum_prof_dep],
  install: true,
)

pkg = import('pkgconfig')
pkg.generate(filebase: 'frida-gumpp-' + api_version,
  name: 'Gum++',
  version: gum_version,
  description: 'Gum C++ bindings',
  requires_private: ['frida-gum-' + api_version],
  subdirs: install_header_basedir,
  libraries: [gumpp],
)
