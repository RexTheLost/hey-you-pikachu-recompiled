package com.hyprecomp.pikachu;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * On-screen N64 controller drawn over the game (landscape). The game picture is 4:3 in the
 * middle; the controls sit in the side bars:
 *   left:  control stick, L / Start / Words
 *   right: TALK (the N64's Z button: hold it and speak), A, B, the four C buttons, R
 * Every finger is tracked on its own, so the stick and buttons work at the same time.
 * A finger that slides off a button onto another one moves the press with it (like a d-pad).
 */
public class ControlsView extends View {
    private static class Btn {
        final String label; final int bits; final float fx, fy, fr; final int color; final boolean action;
        float x, y, r;
        Btn(String label, int bits, float fx, float fy, float fr, int color, boolean action) {
            this.label = label; this.bits = bits; this.fx = fx; this.fy = fy; this.fr = fr;
            this.color = color; this.action = action;
        }
    }

    private static final int WORDS = -1, HIDE = -2;     // "bits" of the non-N64 buttons

    private final HypActivity activity;
    private final List<Btn> buttons = new ArrayList<>();
    private final SparseArray<Btn> fingerButton = new SparseArray<>();
    private int stickFinger = -1;
    private float stickX, stickY, stickR, knobX, knobY;  // base centre / radius, knob position
    private boolean hidden;
    private int lastBits = -1;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
        text = new Paint(Paint.ANTI_ALIAS_FLAG), banner = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Vibrator vibrator;
    private final SharedPreferences prefs;
    private String heard = "";
    private int micStatus = 0;
    private boolean listening;

    public ControlsView(HypActivity a) {
        super(a);
        activity = a;
        prefs = a.getSharedPreferences("controls", Context.MODE_PRIVATE);
        hidden = prefs.getBoolean("hidden", false);
        vibrator = (Vibrator) a.getSystemService(Context.VIBRATOR_SERVICE);
        line.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(Color.WHITE);
        text.setFakeBoldText(true);
        banner.setTextAlign(Paint.Align.CENTER);
        banner.setFakeBoldText(true);
        setFocusable(false);

        // Positions: fractions of the screen HEIGHT h, from the left edge (fx >= 0) or
        // from the right edge (fx < 0: x = width + fx*h). Radii are fractions of h too.
        int yellow = Color.rgb(245, 200, 40), blue = Color.rgb(70, 110, 230), green = Color.rgb(40, 160, 80),
            grey = Color.rgb(120, 120, 130), c = Color.rgb(230, 190, 30);
        buttons.add(new Btn("TALK", HypNative.Z, -0.22f, 0.80f, 0.135f, Color.rgb(230, 70, 60), false));
        buttons.add(new Btn("A", HypNative.A, -0.11f, 0.50f, 0.080f, blue, false));
        buttons.add(new Btn("B", HypNative.B, -0.30f, 0.56f, 0.072f, green, false));
        buttons.add(new Btn("▲", HypNative.CU, -0.22f, 0.12f, 0.050f, c, false));
        buttons.add(new Btn("▼", HypNative.CD, -0.22f, 0.32f, 0.050f, c, false));
        buttons.add(new Btn("◀", HypNative.CL, -0.32f, 0.22f, 0.050f, c, false));
        buttons.add(new Btn("▶", HypNative.CR, -0.12f, 0.22f, 0.050f, c, false));
        buttons.add(new Btn("R", HypNative.R, -0.42f, 0.08f, 0.052f, grey, false));
        buttons.add(new Btn("L", HypNative.L, 0.08f, 0.08f, 0.052f, grey, false));
        buttons.add(new Btn("START", HypNative.START, 0.24f, 0.30f, 0.065f, Color.rgb(200, 50, 50), false));
        buttons.add(new Btn("Words", WORDS, 0.36f, 0.10f, 0.062f, yellow, true));
        buttons.add(new Btn("Hide", HIDE, 0.10f, 0.30f, 0.050f, grey, true));
        setWillNotDraw(false);
        post(this::poll);
    }

    /** Refresh what the microphone heard / whether the game listens, a few times a second. */
    private void poll() {
        try {
            String h = HypNative.getHeard();
            int s = HypNative.getMicStatus();
            boolean l = HypNative.isListening();
            if (!h.equals(heard) || s != micStatus || l != listening) {
                heard = h; micStatus = s; listening = l;
                invalidate();
            }
        } catch (UnsatisfiedLinkError e) {
            // libmain.so not loaded yet
        }
        postDelayed(this::poll, 200);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        for (Btn b : buttons) {
            b.x = b.fx >= 0 ? b.fx * h : w + b.fx * h;
            b.y = b.fy * h;
            b.r = b.fr * h;
        }
        stickX = 0.22f * h;
        stickY = 0.70f * h;
        stickR = 0.17f * h;
        knobX = stickX;
        knobY = stickY;
    }

    private Btn hit(float x, float y) {
        Btn best = null;
        float bestD = Float.MAX_VALUE;
        for (Btn b : buttons) {
            if (hidden && b.bits != HIDE) continue;
            float d = (float) Math.hypot(x - b.x, y - b.y);
            if (d < b.r * 1.25f && d < bestD) {   // a little slack around each button
                best = b;
                bestD = d;
            }
        }
        return best;
    }

    private boolean inStick(float x, float y) {
        return !hidden && Math.hypot(x - stickX, y - stickY) < stickR * 1.6f;
    }

    private void moveStick(float x, float y) {
        float dx = x - stickX, dy = y - stickY;
        float len = (float) Math.hypot(dx, dy);
        if (len > stickR) {
            dx *= stickR / len;
            dy *= stickR / len;
        }
        knobX = stickX + dx;
        knobY = stickY + dy;
        float nx = dx / stickR, ny = -dy / stickR;
        if (Math.hypot(nx, ny) < 0.12) { nx = 0; ny = 0; }   // dead zone
        HypNative.setStick(nx, ny);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int idx = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int id = e.getPointerId(idx);
                float x = e.getX(idx), y = e.getY(idx);
                Btn b = hit(x, y);
                if (b != null) {
                    if (b.action) {
                        if (b.bits == WORDS) activity.showWords();
                        else if (b.bits == HIDE) {
                            hidden = !hidden;
                            prefs.edit().putBoolean("hidden", hidden).apply();
                        }
                        buzz();
                    } else {
                        fingerButton.put(id, b);
                    }
                } else if (stickFinger < 0 && inStick(x, y)) {
                    stickFinger = id;
                    moveStick(x, y);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    float x = e.getX(i), y = e.getY(i);
                    if (id == stickFinger) {
                        moveStick(x, y);
                    } else if (fingerButton.get(id) != null) {
                        Btn now = hit(x, y);
                        if (now != null && !now.action && now != fingerButton.get(id)) fingerButton.put(id, now);
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (action == MotionEvent.ACTION_CANCEL || action == MotionEvent.ACTION_UP) {
                    fingerButton.clear();
                    stickFinger = -1;
                } else {
                    int id = e.getPointerId(idx);
                    fingerButton.remove(id);
                    if (id == stickFinger) stickFinger = -1;
                }
                if (stickFinger < 0) {
                    knobX = stickX;
                    knobY = stickY;
                    HypNative.setStick(0, 0);
                }
                break;
            }
        }
        int bits = 0;
        for (int i = 0; i < fingerButton.size(); i++) bits |= fingerButton.valueAt(i).bits;
        if (bits != lastBits) {
            if ((bits & ~Math.max(lastBits, 0)) != 0) buzz();   // a new button went down
            lastBits = bits;
            HypNative.setButtons(bits);
        }
        invalidate();
        return true;
    }

    private void buzz() {
        if (vibrator != null && vibrator.hasVibrator()) {
            vibrator.vibrate(VibrationEffect.createOneShot(12, 60));
        }
    }

    private boolean held(Btn b) {
        for (int i = 0; i < fingerButton.size(); i++) if (fingerButton.valueAt(i) == b) return true;
        return false;
    }

    @Override
    protected void onDraw(Canvas c) {
        float h = getHeight();
        // What the microphone heard / mic state, top centre over the game.
        String msg = null;
        int msgColor = Color.WHITE;
        if (!heard.isEmpty()) { msg = "heard: " + heard; }
        else if (lastBits > 0 && (lastBits & HypNative.Z) != 0) {
            msg = micStatus == 1 ? "listening... speak now" : micStatus == 0 ? "voice recognizer still loading" : "no microphone: use Words";
            msgColor = micStatus == 1 ? Color.rgb(255, 230, 120) : Color.rgb(255, 150, 140);
        }
        if (msg != null) {
            banner.setTextSize(0.05f * h);
            float w = banner.measureText(msg) / 2 + 0.03f * h;
            fill.setColor(Color.argb(150, 0, 0, 0));
            c.drawRoundRect(new RectF(getWidth() / 2f - w, 0.86f * h, getWidth() / 2f + w, 0.95f * h), 0.03f * h, 0.03f * h, fill);
            banner.setColor(msgColor);
            c.drawText(msg, getWidth() / 2f, 0.925f * h, banner);
        }

        for (Btn b : buttons) {
            if (hidden && b.bits != HIDE) continue;
            boolean on = held(b);
            fill.setColor(b.color);
            fill.setAlpha(on ? 230 : (b.bits == HypNative.Z ? 150 : 110));
            c.drawCircle(b.x, b.y, b.r, fill);
            line.setColor(Color.argb(on ? 255 : 150, 255, 255, 255));
            line.setStrokeWidth(0.006f * h);
            c.drawCircle(b.x, b.y, b.r, line);
            String label = b.bits == HIDE && hidden ? "Show" : b.label;
            text.setTextSize(b.r * (label.length() > 2 ? 0.55f : 0.9f));
            text.setAlpha(on ? 255 : 210);
            c.drawText(label, b.x, b.y - (text.descent() + text.ascent()) / 2, text);
        }
        if (!hidden) {
            fill.setColor(Color.argb(70, 255, 255, 255));
            c.drawCircle(stickX, stickY, stickR, fill);
            line.setColor(Color.argb(140, 255, 255, 255));
            c.drawCircle(stickX, stickY, stickR, line);
            fill.setColor(Color.argb(stickFinger >= 0 ? 230 : 160, 230, 230, 235));
            c.drawCircle(knobX, knobY, stickR * 0.45f, fill);
        }
    }
}
