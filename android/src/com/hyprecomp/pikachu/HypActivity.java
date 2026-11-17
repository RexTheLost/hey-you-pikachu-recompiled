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
    private final MicRecorder mic = new MicRecorder();

    @Override
    protected void onResume() {
        super.onResume();
        if (!mBrokenLibraries && checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                == android.content.pm.PackageManager.PERMISSION_GRANTED) {
            mic.start();
        }
    }

    @Override
    protected void onPause() {
        mic.stop();
        super.onPause();
    }

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
        controls = new ControlsView(this);
        mLayout.addView(controls, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        hideBars();
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
            .setNeutralButton("Copy logs", (d, which) -> copyVoiceLog())
            .setNegativeButton("Close", null)
            .show();
    }

    /** Puts the end of data/voice.log on the clipboard, to paste into a bug report. */
    void copyVoiceLog() {
        String text;
        try {
            text = "voice log:\n" + LauncherActivity.readText(new java.io.File(getFilesDir(), "data/voice.log"), 5000)
                + "\n--- game log ---\n" + LauncherActivity.readText(new java.io.File(getFilesDir(), LauncherActivity.LOG), 5000);
        } catch (Exception e) {
            text = "no voice log: " + e;
        }
        android.content.ClipboardManager cm = (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        cm.setPrimaryClip(android.content.ClipData.newPlainText("voice log", text));
        android.widget.Toast.makeText(this, "Logs copied", android.widget.Toast.LENGTH_SHORT).show();
    }
}
