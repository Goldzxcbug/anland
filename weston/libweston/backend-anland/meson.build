if not get_option('backend-anland')
	subdir_done()
endif

config_h.set('BUILD_ANLAND_COMPOSITOR', '1')

anland_root = join_paths(meson.project_source_root(), '..')

srcs_anland = [
	'anland.c',
	join_paths(anland_root, 'libdisplay_producer', 'display_producer.c'),
	join_paths(anland_root, 'common', 'socket_utils.c'),
	presentation_time_server_protocol_h,
]

plugin_anland = shared_library(
	'anland-backend',
	srcs_anland,
	include_directories: common_inc,
	c_args: [
		'-I' + join_paths(anland_root, 'libdisplay_producer'),
		'-I' + join_paths(anland_root, 'common'),
		'-O3',
		'-flto',
	],
	link_args: ['-flto'],
	dependencies: [
		dep_libweston_private,
		dep_libdrm_headers,
		dep_lib_cairo_shared,
	],
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston,
)
env_modmap += 'anland-backend.so=@0@;'.format(plugin_anland.full_path())
install_headers(backend_anland_h, subdir: dir_include_libweston_install)
