package com.hyprecomp.pikachu;

import android.app.AlertDialog;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

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
            .setNegativeButton("Close", null)
            .show();
    }
}
