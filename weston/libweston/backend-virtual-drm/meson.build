if not get_option('backend-virtual-drm')
	subdir_done()
endif

config_h.set('BUILD_VIRTUAL_DRM_COMPOSITOR', '1')

vdrm_root = join_paths(meson.project_source_root(), '..')

srcs_vdrm = [
	'virtual-drm.c',
	presentation_time_server_protocol_h,
]

plugin_vdrm = shared_library(
	'virtual-drm-backend',
	srcs_vdrm,
	include_directories: common_inc,
	c_args: [
		'-I' + join_paths(vdrm_root, 'libdisplay_producer'),
		'-I' + join_paths(vdrm_root, 'common'),
	],
	link_args: [
		'-L' + join_paths(vdrm_root, get_option('vdrm-lib-dir')),
		'-ldisplay_producer',
		'-lsocket_utils',
	],
	dependencies: [
		dep_libweston_private,
		dep_libdrm_headers,
		dep_lib_cairo_shared,
	],
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston,
)
env_modmap += 'virtual-drm-backend.so=@0@;'.format(plugin_vdrm.full_path())
install_headers(backend_virtual_drm_h, subdir: dir_include_libweston_install)
