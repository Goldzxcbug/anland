#ifndef WESTON_COMPOSITOR_VIRTUAL_DRM_H
#define WESTON_COMPOSITOR_VIRTUAL_DRM_H

#ifdef  __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <libweston/libweston.h>

#define WESTON_VIRTUAL_DRM_BACKEND_CONFIG_VERSION 1

struct weston_virtual_drm_backend_config {
	struct weston_backend_config base;
	const char *socket_path;
	int refresh; /* mHz, 0 = 60000 */
	enum weston_renderer_type renderer;
};

#ifdef  __cplusplus
}
#endif

#endif /* WESTON_COMPOSITOR_VIRTUAL_DRM_H */
