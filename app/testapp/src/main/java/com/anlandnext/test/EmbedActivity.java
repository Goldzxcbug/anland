package com.anlandnext.test;

import android.os.Bundle;
import android.util.Log;

import com.anlandnext.awl.Awl;
import com.anlandnext.awl.AwlWindowActivity;

import java.io.BufferedReader;
import java.io.FileDescriptor;
import java.io.InputStreamReader;

/**
 * Demo of SUBCLASSING the library's window activity with LATE BINDING: this
 * activity starts with NO window id — it launches the wayland app itself,
 * waits for the created event, then hosts the window in THIS instance
 * (hostWindow). One activity = its own embedded wayland app.
 *
 * (super.onCreate cannot be deferred until the window exists — the platform
 * lifecycle waits for no async event — so the library binds internally and
 * hosting proceeds identically afterwards.)
 */
public class EmbedActivity extends AwlWindowActivity {
    private static final String TAG = "anland-embed";

    @Override
    protected boolean onAwaitWindow() {
        return true;   /* started without an id: stay alive, wait for hostWindow */
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);   /* builds the UI unbound, awaiting */

        FileDescriptor fd = Awl.getWaylandFd();
        if (fd == null) {
            Log.e(TAG, "no wayland fd (daemon down?) → finish");
            finish();
            return;
        }

        /* the first window our client creates → THIS activity hosts it */
        final Awl.Callback cb = new Awl.Callback() {
            @Override public void onWindowCreated(long id, String title) {
                Log.i(TAG, "window " + id + " '" + title + "' → hosting in-place");
                Awl.unregisterCallback(this);
                hostWindow(id, title, null);
            }
            @Override public void onWindowDestroyed(long id) { }
            @Override public void onWindowAttached(long id) { }
            @Override public void onWindowDetached(long id) { }
        };
        Awl.registerCallback(cb);

        String exe = getApplicationInfo().nativeLibraryDir + "/libawlshm.so";
        Awl.ClientProcess p = Awl.spawnClient(fd, exe);
        if (p == null) {
            Log.e(TAG, "spawn failed → finish");
            Awl.unregisterCallback(cb);
            finish();
            return;
        }
        Log.i(TAG, "client pid=" + p.pid + ", waiting for its window");
        drain(p.stdout, "out");
        drain(p.stderr, "err");
    }

    private void drain(android.os.ParcelFileDescriptor pfd, String tag) {
        new Thread(() -> {
            try (BufferedReader r = new BufferedReader(
                    new InputStreamReader(new java.io.FileInputStream(pfd.getFileDescriptor())))) {
                String line;
                while ((line = r.readLine()) != null)
                    Log.i("awlshm-" + tag, line);
            } catch (Exception ignored) { }
        }, "awlshm-" + tag).start();
    }
}
