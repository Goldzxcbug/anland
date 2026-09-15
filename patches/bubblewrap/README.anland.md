# bubblewrap 0.11.1 (vendored, one fix) — why anland ships its own bwrap

Source: <https://github.com/containers/bubblewrap/releases/tag/v0.11.1>
(bubblewrap.c, bind-mount.[ch], network.[ch], utils.[ch], COPYING — pristine
except for `bind-mount.c`, see `anland-mountinfo-index.patch`). License:
LGPL-2.0-or-later (COPYING). `config.h` is ours (meson would generate it).

In the anland repo this directory is `patchs/bubblewrap/`; `make anlandx`
copies the pristine submodule (`third_party/bubblewrap`, pinned at v0.11.1)
and applies `anland-mountinfo-index.patch` to the staged copy before tarring
— the submodule itself is never modified (same scheme as `pulse/termux/`).

## The bug it works around

Devices rooted with KernelSU + SuSFS give every mount created or cloned by a
KernelSU-domain process a *fake* mount id counted from
`DEFAULT_KSU_MNT_ID = 2000000000` (susfs `kernel_patches/include/linux/susfs_def.h`).
droidspaces is started through KernelSU `su`, so inside an anland container
`/proc/self/mountinfo` is full of ids around 2 000 000 000.

bubblewrap's `parse_mountinfo()` (bind-mount.c) indexes mounts by raw id:

    by_id = xcalloc (max_id + 1, sizeof (MountInfoLine*));   /* 2e9 × 8 = 16 GB */

That is 16 GB of address space. Normally overcommit lets it through, but
glycin (the sandboxed image loader behind gdk-pixbuf 2.44 / GTK 3.24.5x /
GTK 4 on Ubuntu 26.04) launches its loaders with
`setrlimit(RLIMIT_AS, 0.8 × min(MemAvailable + SwapFree, 20 GB))` in a
`pre_exec` hook right before exec'ing `bwrap` (glycin/src/sandbox.rs
`set_memory_limit`). The cap of 20 GB × 0.8 = 16 GB is below what the table
needs, so on such a device the glycin sandbox can never start:

    Loader stderr: Setting process memory limit
    Loader stderr: Out of memory                 ← bwrap's die_oom()
    Process exited: Some(Some(1))

Every SVG icon load then fails. GTK 3 treats a failing `image-missing`
fallback as fatal (`gtkiconhelper.c` `ensure_surface_for_gicon`:
`g_assert_no_error`) → the process aborts. Visible symptom: VS Code (Electron,
GTK file chooser / message boxes) dies the moment a native dialog opens.

## The fix

`parse_mountinfo()` keeps an array of `n_lines` pointers sorted by id and
looks parents up with `bsearch` — memory proportional to the number of
mounts, not to the largest id. Behaviour is otherwise identical (mount ids
are unique within a namespace, so the "last writer wins" of the old table
never mattered), which is why the fixed lookup is compiled in
unconditionally: the same binary is correct on normal kernels too, and
switching kernels (SuSFS ↔ plain) can never invalidate an installed copy.

## How it is deployed

`anland-session/setup.sh` (setupanlandx.sh in the tarball) always compiles
these sources inside the container (`~/.local/bin/bwrap`; deps.sh installs
the build-dep set via `apt-get build-dep bubblewrap` — no per-device SuSFS
detection is needed), and `anland-session.sh` publishes `PATH=$HOME/.local/bin:…` into the systemd
user environment so apps launched through `systemd-run --user`
(anland-shell) resolve `bwrap` to it — glycin finds bwrap via `PATH`
(`Command::new("bwrap")`). `setupanlandx.sh --uninstall` removes it again.

Upstream bubblewrap `main` (checked 2026-09-15, after v0.12.0) still uses the
id-indexed table; the patch is worth sending upstream.
