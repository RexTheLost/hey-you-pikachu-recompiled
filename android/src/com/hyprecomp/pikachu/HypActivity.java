package com.hyprecomp.pikachu;

import android.app.AlertDialog;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** The game: SDL's activity (it runs libmain.so) with the on-screen controls laid over it. */
public class HypActivity extends SDLActivity {
    private ControlsView controls;

    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle state) {
        // A Java crash in the game screen goes into the same report as native ones.
        final Thread.UncaughtExceptionHandler old = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            try (java.io.PrintWriter w = new java.io.PrintWriter(new java.io.FileWriter(
                    new java.io.File(getFilesDir(), LauncherActivity.CRASH), true))) {
                w.println("Java exception in thread " + t.getName() + ":");
                e.printStackTrace(w);
            } catch (Exception ignored) { }
            if (old != null) old.uncaughtException(t, e);
        });
        // The game window covers the whole screen: no space kept for the status bar, the
        // gesture bar or the camera hole (otherwise the 4:3 picture is shorter than the screen).
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        } else if (android.os.Build.VERSION.SDK_INT >= 28) {
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        getWindow().setAttributes(lp);
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
        }
        super.onCreate(state);
        if (mBrokenLibraries || mLayout == null) return;
        if (HypNative.isVrPackage(this)) {
            // Quest: the controllers and the VR panel replace the touch controls.
            Thread t = new Thread(this::postVrReports, "vr-reports");
            t.setDaemon(true);
            t.start();
            return;
        }
        controls = new ControlsView(this);
        mLayout.addView(controls, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        hideBars();
    }

    /** Quest: posts the spoken bug reports the VR panel queues (HypNative.takeReport) to the report inbox. */
    private void postVrReports() {
        while (true) {
            try { Thread.sleep(500); } catch (InterruptedException e) { return; }
            String json;
            try { json = HypNative.takeReport(); } catch (Throwable t) { continue; }
            if (json == null) continue;
            boolean ok = false;
            String message;
            try {
                java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(Updater.SERVER + "/report").openConnection();
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setConnectTimeout(15000);
                c.setReadTimeout(15000);
                c.setRequestProperty("Content-Type", "application/json");
                c.setRequestProperty("X-HYP-App", "hyp-android");
                c.getOutputStream().write(json.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                int code = c.getResponseCode();
                java.io.InputStream in = code < 400 ? c.getInputStream() : c.getErrorStream();
                StringBuilder body = new StringBuilder();
                if (in != null) {
                    byte[] buf = new byte[4096];
                    int n;
                    while ((n = in.read(buf)) > 0) body.append(new String(buf, 0, n, java.nio.charset.StandardCharsets.UTF_8));
                }
                String b = body.toString();
                ok = code == 200 && b.contains("\"ok\":true");
                String id = "";
                int p = b.indexOf("\"id\":");
                if (p >= 0) id = b.substring(p + 5).replaceAll("[^0-9].*$", "");
                message = ok ? "Report #" + id + " is in the inbox." : "The report inbox said no (" + code + ").";
            } catch (Exception e) {
                message = "No connection to the report inbox. Try again later.";
            }
            try { HypNative.reportResult(ok, message); } catch (Throwable ignored) { }
        }
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideBars();
    }

    private void hideBars() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    /** The word list: the words the game understands right now; tapping one says it. */
    void showWords() {
        String[] raw = HypNative.getWords();
        if (raw == null || raw.length == 0) {
            new AlertDialog.Builder(this).setTitle("Words")
                .setMessage("Pikachu isn't listening for words yet. Try again once the game is running.")
                .setPositiveButton("OK", null).show();
            return;
        }
        Arrays.sort(raw);                                  // grouped: "group\tword"
        final List<String> words = new ArrayList<>();
        List<String> shown = new ArrayList<>();
        String group = null;
        for (String r : raw) {
            int tab = r.indexOf('\t');
            String g = tab >= 0 ? r.substring(0, tab) : "";
            String w = tab >= 0 ? r.substring(tab + 1) : r;
            if (!g.equals(group)) {
                group = g;
                words.add(null);                           // a header row
                shown.add("— " + g + " —");
            }
            words.add(w);
            shown.add("    " + w);
        }
        String set = HypNative.getSetName();
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(set == null || set.isEmpty() ? "Say a word" : "Say a word (" + set + ")")
            .setItems(shown.toArray(new String[0]), (d, which) -> {
                String w = words.get(which);
                if (w != null) HypNative.sayWord(w);
                else showWords();                          // header tapped: keep the list open
            })
            .setNeutralButton("Send logs to Claude", (d, which) -> sendLogs())
            .setPositiveButton(LiveLog.on ? "Live log: ON" : "Live log: off", (d, which) -> {
                if (LiveLog.on) LiveLog.stop(); else LiveLog.start(this);
                android.widget.Toast.makeText(this, LiveLog.on
                    ? "Live log on: the game log and your Talk recordings go to Claude until you turn it off"
                    : "Live log off", android.widget.Toast.LENGTH_LONG).show();
            })
            .setNegativeButton("Close", null)
            .show();
    }

    /**
     * Sends the current game and voice logs to the report inbox (for problems that don't crash,
     * like crackling sound), and copies them too in case there is no internet.
     */
    void sendLogs() {
        final String text = "Logs sent from the game on request\n\n--- game log ---\n"
            + LauncherActivity.readText(new java.io.File(getFilesDir(), LauncherActivity.LOG), 30000)
            + "\n--- voice log ---\n"
            + LauncherActivity.readText(new java.io.File(getFilesDir(), "data/voice.log"), 10000);
        android.content.ClipboardManager cm = (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        cm.setPrimaryClip(android.content.ClipData.newPlainText("logs", text));
        String version = "?";
        try { version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception ignored) { }
        final String v = version;
        android.widget.Toast.makeText(this, "Sending logs...", android.widget.Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            boolean ok = LauncherActivity.postReport("logs", v, text);
            runOnUiThread(() -> android.widget.Toast.makeText(this,
                ok ? "Logs sent to Claude" : "Could not send (no internet?). The logs are copied: paste them instead.",
                android.widget.Toast.LENGTH_LONG).show());
        }).start();
    }
}
