/* config.h — anland stand-in for the file meson generates when building
 * bubblewrap. Only PACKAGE_STRING is needed; HAVE_SELINUX stays undefined
 * (no --exec-label/--file-label support, glycin does not use them) and
 * ENABLE_REQUIRE_USERNS stays undefined (the binary is not setuid: it runs
 * unprivileged with user namespaces, like the distro build in the container).
 * See README.anland.md. */
#define PACKAGE_STRING "bubblewrap 0.11.1 (anland: mountinfo index fix)"
