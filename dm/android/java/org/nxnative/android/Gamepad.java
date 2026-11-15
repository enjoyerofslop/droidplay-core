// The app's Java side (front_android.c loads it through the activity's class loader and registers the natives): the virtual gamepad, the software
// keyboard's text box, the shake sensor.
// The virtual gamepad is dm/ios/Sources/DmGamepad.m on Android (its file header has the design; the constants, layouts and touch ownership are the same, in dp for points):
//   2D (simple), 3D (full, with D-pad, L3 / R3 and a right stick in the right third) and Tap (no controls: touches go to the game's touch screen; handheld mode only),
//   cycled by the layout button at the top centre and kept per app (each game is its own app). Touches no control takes go to the game's touch screen, normalized
//   on the centred 16:9 picture. The controls hide while a physical controller is connected (its keys reach the native input queue: this window is not focusable).
// The ≡ button (shown with a controller too) opens the pause menu, drawn here over the held game: Resume, the touch layout, the frame-rate counter, back to the launcher.
// NativeActivity owns its window's surface (no view of its own is drawn), so the gamepad is a PopupWindow over the whole activity.
// Shaking the phone shakes the controller (the accelerometer: gravity followed by a slow low-pass, a hit past DM_SHAKE_G, 1.5 G by default).
package org.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.text.InputFilter;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.TextView;
import java.util.concurrent.CountDownLatch;

public final class Gamepad extends View {
    static native void nativeTouchPad(long buttons, int lx, int ly, int rx, int ry);   // the controls' state (front_android.c merges it with a physical controller's)
    static native void nativeGameTouch(int[] ids, float[] xs, float[] ys, int n);      // the game's touch screen: dm_touch_set
    static native void nativePicture(float[] out);   // the picture's area in the window (DM_PICTURE_TOP, DM_PICTURE_HEIGHT): {top, height}, shares of the window
    static native void nativeShake(boolean on);
    static native int nativeOpMode();          // hle_oe.c dm_opmode: 0 handheld
    static native int nativeDefaultLayout();   // the game profile's pad_layout (DM_GAME_PAD_LAYOUT)
    static native void nativeLog(String s);
    static native String nativeEnv(String name);   // the host's environment (env.txt is read after the VM starts: System.getenv does not see it)
    static native long nativeFrames();         // frames the game presented (front_android.c): the frame-rate counter
    static native boolean nativePause(boolean on);   // the pause menu opened or closed: the game held or let go (front_android.c); false: no game is running

    static final int B_A = 0, B_B = 1, B_X = 2, B_Y = 3, B_LS = 4, B_RS = 5, B_L = 6, B_R = 7, B_ZL = 8, B_ZR = 9, B_PLUS = 10, B_MINUS = 11,
                     B_LEFT = 12, B_UP = 13, B_RIGHT = 14, B_DOWN = 15, B_N = 16;   // the bits of dm_input.h
    static final int C_NONE = 0, C_STICK = 1, C_DPAD = 2, C_FACE = 3, C_BTN = 4, C_SWAP = 5, C_GAME = 6, C_MENU = 7;
    // DmGamepad.m's layout constants (in u; x from the outer edge, the right half mirrors)
    static final float K_ZL_X = 44, K_ZL_Y = 40, K_ZL_R = 26, K_L_X = 44, K_L_Y = 108, K_L_R = 24, K_DROP_TIP = 1.45f, K_CORNER_IN = 0.5f,
        K_FACE_X = 81, K_PAD_Y = 82, K_FACE_GAP = 50, K_FACE_R = 27, K_PILL_DX = 52, K_PILL_Y = 22, K_PILL_W = 50, K_PILL_H = 28, K_SWAP_R = 15,
        K_STICK_X = 81, K_STICK_R = 77, K_KNOB_R = 30, K_STICK_GRAB = 1.4f, K_FULL = 0.8f, K_DEAD = 0.1f,
        K_LOW_X = 231, K_LOW_Y = 64, K_DPAD_R = 48, K_ARM_W = 17, K_ARM_IN = 11, K_DPAD_GRAB = 1.3f, K_RSTICK_R = 60, K_RSWIPE = 1f / 3, K_CLICK_X = 102, K_CLICK_R = 17,
        // the diamond's touch area past its drawn buttons, in face radii: 3D gives what's past them to the camera, so a swipe that starts beside the buttons turns it (#433)
        K_FACE_GRAB = 1.6f, K_FACE_GRAB_3D = 1.15f;
    static final int[] K_BTNS = {B_ZL, B_L, B_ZR, B_R, B_MINUS, B_PLUS, B_LS, B_RS};
    static final String[] NAMES = {"A", "B", "X", "Y", "L3", "R3", "L", "R", "ZL", "ZR", "+", "−", null, null, null, null};

    final float dp; final SharedPreferences prefs;
    boolean full, tap, padOff;
    // Folded open with the gamepad below the game (DM_PAD_HALF=1): the picture keeps the top of the window (front_android.c picture_range) and the controls are laid out in the
    // band under it. padTop is that band's top edge in the view's y: every vertical reference below measures from it, and a touch above it is the picture's, which goes to the
    // game's touch screen as normalized on the picture there. Off by default: every number below then measures from 0 and the whole window, as before
    boolean half; float padTop; float picTop, picH = 1;
    // touches by pointer id: owner (control | index << 8), the diamond keys each holds
    final long[] owner = new long[64]; final long[] faceOf = new long[64]; final boolean[] down = new boolean[64];
    final float[] px = new float[64], py = new float[64];
    int[] stick = {-1, -1}; float[] stickX = new float[2], stickY = new float[2]; float rcx, rcy;
    long faceBits, dpadBits, btnBits; boolean swapHeld, menuHeld, menuGo; int hapticSticks;
    final int[] game = new int[16];   // pointer id holding each finger slot of the game's touch screen, -1 free
    final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    static final int IDLE = Color.argb(36, 255, 255, 255), ON = Color.argb(115, 255, 255, 255), LINE = Color.argb(97, 255, 255, 255),
                     RING = Color.argb(15, 255, 255, 255), TEXT = Color.argb(153, 255, 255, 255), TEXT_ON = Color.argb(179, 0, 0, 0);
    float insL, insR, insT, insB;
    long lastBtn; int[] lastAx = new int[4];   // what was last sent (the self-check reads it)

    Gamepad(Context c) {
        super(c);
        dp = c.getResources().getDisplayMetrics().density;
        prefs = c.getSharedPreferences("nxnative", Context.MODE_PRIVATE);
        java.util.Arrays.fill(game, -1);
        line.setStyle(Paint.Style.STROKE); line.setStrokeWidth(1.5f * dp); line.setColor(LINE);
        fill.setStyle(Paint.Style.FILL);
        text.setTextAlign(Paint.Align.CENTER); text.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        int l = prefs.getInt("DmPadLayout", nativeDefaultLayout());
        if (l == 2 && nativeOpMode() != 0) l = 0;
        full = l == 1; tap = l == 2;
        setHapticFeedbackEnabled(true);
    }

    // ---- Layout (px; the safe area from the window's cutout insets) ----
    @Override public WindowInsets onApplyWindowInsets(WindowInsets in) {
        if (android.os.Build.VERSION.SDK_INT >= 28 && in.getDisplayCutout() != null) {
            android.view.DisplayCutout c = in.getDisplayCutout();
            insL = c.getSafeInsetLeft(); insR = c.getSafeInsetRight(); insT = c.getSafeInsetTop(); insB = c.getSafeInsetBottom();
        }
        layoutBand();   // the band's top (half mode) is a share of the height, known once the view has one
        invalidate(); return in;
    }
    // The player's size and inset (droidplay-core#222: a stick too close to the left edge of a Poco F7): DM_PAD_SCALE multiplies u, and DM_PAD_INSET (0 to 1) moves the
    // stick, the D-pad and the face buttons in from the sides by that share of the room between their touch areas, and DM_PAD_RAISE (0 to 1) moves them up from the bottom
    // by that share of the room between their touch areas and the shoulder buttons' (the tester's "a bit higher"). Set per game in the launcher (launcher.txt) or in env.txt;
    // 1 and 0 are the standard layout. A size over 1 grows only while the corner groups stay clear of the shoulder buttons and of each other; both are measured with the 3D
    // layout's reach, so switching layouts neither resizes nor moves them
    float scaleSet = 1, insetSet = 0, raiseSet = 0;
    static final float SCALE_MIN = 0.6f, SCALE_MAX = 1.4f, K_GAP = 12,
        K_LEFT_REACH = K_LOW_X + K_DPAD_R * K_DPAD_GRAB, K_FACE_REACH = K_FACE_X + K_FACE_GAP + K_FACE_R * 1.25f,   // the corner groups' touch areas from their corners (faceAt, dpadAt)
        K_UP_REACH = K_PAD_Y + K_STICK_R * K_STICK_GRAB, K_DROP_REACH = K_L_Y + K_L_R * 1.35f;   // a bottom group's up from the bottom, and a shoulder button's down from the top (btnAt)
    void setPadSize(String scale, String inset, String raise) {
        try { if (scale != null) scaleSet = Math.max(SCALE_MIN, Math.min(SCALE_MAX, Float.parseFloat(scale))); } catch (NumberFormatException e) {}
        try { if (inset != null) insetSet = Math.max(0f, Math.min(1f, Float.parseFloat(inset))); } catch (NumberFormatException e) {}
        try { if (raise != null) raiseSet = Math.max(0f, Math.min(1f, Float.parseFloat(raise))); } catch (NumberFormatException e) {}
    }
    // In half mode the band's height stands in for the window's: every vertical reference (the bottom the controls sit on, the height they are measured against) is the
    // band's, and a control's y is the band's top plus its offset in it. Whole-window mode: the window's own, exactly as before
    float padH() { return half ? getHeight() - padTop : getHeight(); }
    float baseU() { return dp * Math.min(padH() / dp / 430f, 1.3f); }
    float padScale() {   // scaleSet, a size over 1 held where the corner groups would meet the shoulder buttons or each other
        if (scaleSet <= 1) return scaleSet;
        float b = baseU(); if (b <= 0) return 1;
        float fit = Math.min(bandH() / ((K_UP_REACH + K_DROP_REACH + K_GAP) * b), (cornerR0() - cornerL0()) / ((K_LEFT_REACH + K_FACE_REACH + K_GAP) * b));
        return Math.max(1f, Math.min(scaleSet, fit));
    }
    float u() { return baseU() * padScale(); }
    float insetPx() {   // each side's share of the room between the left group's touch area and the face buttons'
        if (insetSet <= 0) return 0;
        float free = (cornerR0() - cornerL0()) - (K_LEFT_REACH + K_FACE_REACH + K_GAP) * u();
        return free > 0 ? insetSet * free / 2 : 0;
    }
    float raisePx() {   // the share of the room between the bottom groups' touch areas and the shoulder buttons' (the 3D reach, as padScale measures it) they move up by
        if (raiseSet <= 0) return 0;
        float free = bandH() - (K_UP_REACH + K_DROP_REACH + K_GAP) * u();
        return free > 0 ? raiseSet * free : 0;
    }
    // the band's height past the safe area it starts at: subtracts safeT first so these read as one expression, cornerB0() - safeT() or either way in half mode
    float bandH() { return cornerB0() - safeT(); }
    float safeL() { return insL; } float safeR() { return getWidth() - insR; } float safeT() { return half ? padTop + insT : insT; }
    float cornerL0() { return insL * K_CORNER_IN; } float cornerR0() { return getWidth() - insR * K_CORNER_IN; }
    float cornerL() { return cornerL0() + insetPx(); } float cornerR() { return cornerR0() - insetPx(); }
    float cornerB0() { return getHeight() - insB; } float cornerB() { return cornerB0() - raisePx(); }
    float faceCx() { return cornerR() - K_FACE_X * u(); } float faceCy() { return cornerB() - K_PAD_Y * u(); }
    float faceGap() { return K_FACE_GAP * u(); } float faceR() { return K_FACE_R * u(); }
    float faceX(int b) { return b == B_A ? faceCx() + faceGap() : b == B_Y ? faceCx() - faceGap() : faceCx(); }
    float faceY(int b) { return b == B_B ? faceCy() + faceGap() : b == B_X ? faceCy() - faceGap() : faceCy(); }
    float homeX() { return cornerL() + K_STICK_X * u(); } float homeY() { return cornerB() - K_PAD_Y * u(); }
    float stickCx(int i) { return i == 1 ? rcx : homeX(); } float stickCy(int i) { return i == 1 ? rcy : homeY(); }
    float stickR(int i) { return (i == 1 ? K_RSTICK_R : K_STICK_R) * u(); }
    boolean inRightArea(float x) { return x >= getWidth() - getWidth() * K_RSWIPE; }
    float dpadCx() { return cornerL() + K_LOW_X * u(); } float dpadCy() { return cornerB() - K_LOW_Y * u(); } float dpadR() { return K_DPAD_R * u(); }
    static boolean isDrop(int b) { return b == B_ZL || b == B_L || b == B_ZR || b == B_R; }
    static boolean isClick(int b) { return b == B_LS || b == B_RS; }
    static boolean fullOnly(int b) { return b == B_LS || b == B_RS || b >= B_LEFT; }
    float dropCx(int b) { boolean z = b == B_ZL || b == B_ZR, r = b == B_ZR || b == B_R; float dx = (z ? K_ZL_X : K_L_X) * u(); return r ? safeR() - dx : safeL() + dx; }
    float dropCy(int b) { boolean z = b == B_ZL || b == B_ZR; return safeT() + (z ? K_ZL_Y : K_L_Y) * u(); } float dropR(int b) { return (b == B_ZL || b == B_ZR ? K_ZL_R : K_L_R) * u(); }
    float clickCx(int b) { return b == B_RS ? safeR() - K_CLICK_X * u() : safeL() + K_CLICK_X * u(); } float clickCy() { return safeT() + K_ZL_Y * u(); }
    RectF pill(int b) { float w = K_PILL_W * u(), h = K_PILL_H * u(), cx = (safeL() + safeR()) / 2 + (b == B_PLUS ? K_PILL_DX : -K_PILL_DX) * u(), cy = safeT() + K_PILL_Y * u();
        return new RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2); }
    float swapCx() { return (safeL() + safeR()) / 2; } float swapCy() { return safeT() + K_PILL_Y * u(); }
    // the menu button (back to the launcher), left of −: past the − pill's grown hit area, so the two never share a touch
    float menuCx() { return swapCx() - (K_PILL_DX + K_PILL_W / 2 + 14 + K_SWAP_R) * u(); } float menuCy() { return swapCy(); }
    static float dist(float x0, float y0, float x1, float y1) { return (float)Math.hypot(x0 - x1, y0 - y1); }
    // ---- Half mode: the game above, the controls below (DM_PAD_HALF=1) ----
    // The band starts where the picture's own area ends: both come from DM_PAD_HALF and DM_SPLIT as front_android.c's picture_range reads them, so a touch is always handled by
    // the half it is drawn in. Read from the environment the host started with: a change (setSplit) takes effect at the next start
    void layoutBand() {
        half = !tap && !padOff && nativeEnv("DM_PAD_HALF") != null && !nativeEnv("DM_PAD_HALF").equals("0");
        float[] pr = new float[2];
        if (half) { try { nativePicture(pr); } catch (UnsatisfiedLinkError old) { half = false; } }   // a host older than the split has no nativePicture
        picTop = pr[0]; picH = pr[1] > 0 ? pr[1] : 1;
        // The band is a share of the view's height, so it is only known once the view has one: an inset arrives before the first layout (h=0), and the band then waits for
        // onSizeChanged. Left at 0, every control would be laid out in the whole window over a picture squeezed into it
        padTop = half && getHeight() > 0 ? (float)Math.rint(getHeight() * picTop) : 0;
    }
    int splitPct() {   // the split the host started with (DM_SPLIT): a change to it takes effect at the next start
        String s = nativeEnv("DM_SPLIT"); try { int p = s == null ? 0 : Integer.parseInt(s.trim()); return p > 0 ? p : SPLIT_DEFAULT; } catch (NumberFormatException e) { return SPLIT_DEFAULT; }
    }
    // The split, judged once before anything else: the view is laid out again when it moves, which sends the insets again and draws with the new numbers
    void setSplit(int pct) {   // 0 turns it off; otherwise the picture's share of the height, 30 to 70 (SPLIT_MIN / SPLIT_MAX)
        boolean off = pct <= 0;
        pct = off ? SPLIT_DEFAULT : Math.max(SPLIT_MIN, Math.min(SPLIT_MAX, pct));
        prefs.edit().putInt("DmPadSplit", pct).putBoolean("DmPadHalf", !off).apply();
        writeLauncherKeys(new String[][] {{"DM_PAD_HALF", off ? "0" : "1"}, {"DM_SPLIT", String.valueOf(pct)}});
        // the picture is placed when the host starts (front_android.c picture_range): this run keeps the split it started with, and the band follows the picture's area
        layoutBand();
        requestLayout(); invalidate();
        nativeLog("gamepad: the game " + (off ? "over the whole screen again" : "above, the controls below at " + pct + "%") + " for the next start (launcher.txt)");
    }
    int nextSplit(int pct) { return pct <= SPLIT_MIN ? 0 : pct - SPLIT_STEP; }   // on 50% → 45% → … → 30% → off
    // The settings screen's file (Launcher.java saveSettings) for keys it does not carry itself: a line each, kept as a player wrote them. env.txt still wins over these
    void writeLauncherKeys(String[][] keys) {
        String d = nativeEnv("DM_GAME_DIR"); if (d == null || d.isEmpty()) return;
        java.io.File f = new java.io.File(d, "launcher.txt"), t = new java.io.File(d, "launcher.txt.tmp");
        StringBuilder sb = new StringBuilder("# written by the settings screen and the pause menu (Gamepad.java); env.txt wins over these\n");
        try {
            java.util.Set<String> done = new java.util.HashSet<String>();
            java.util.List<String> lines = f.isFile() ? java.nio.file.Files.readAllLines(f.toPath(), java.nio.charset.StandardCharsets.UTF_8) : new java.util.ArrayList<String>();
            for (String l : lines) {
                if (l.startsWith("#") || !l.contains("=")) continue;
                String k = l.substring(0, l.indexOf('='));
                String v = null;
                for (String[] kv : keys) if (kv[0].equals(k)) { v = kv[1]; done.add(k); }
                if (v != null) sb.append(k).append('=').append(v).append('\n');
            }
            for (String[] kv : keys) if (!done.contains(kv[0])) sb.append(kv[0]).append('=').append(kv[1]).append('\n');
            java.nio.file.Files.write(t.toPath(), sb.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            if (!t.renameTo(f)) throw new java.io.IOException("could not replace " + f);
        } catch (Exception x) { nativeLog("gamepad: launcher.txt not written: " + x); }
    }
    static final int SPLIT_MIN = 30, SPLIT_MAX = 70, SPLIT_STEP = 5, SPLIT_DEFAULT = 50;
    int btnAt(float x, float y) {
        for (int b : K_BTNS) {
            if (fullOnly(b) && !full) continue;
            boolean hit = isDrop(b) ? dist(x, y, dropCx(b), dropCy(b)) <= dropR(b) * 1.35f
                        : isClick(b) ? dist(x, y, clickCx(b), clickCy()) <= K_CLICK_R * u() * 1.35f
                        : grow(pill(b), 8 * u()).contains(x, y);
            if (hit) return b;
        }
        return -1;
    }
    static RectF grow(RectF r, float d) { return new RectF(r.left - d, r.top - d, r.right + d, r.bottom + d); }
    boolean inSwap(float x, float y) { return dist(x, y, swapCx(), swapCy()) <= K_SWAP_R * u() * 1.6f; }
    boolean inMenu(float x, float y) { return menuAlpha() > 0 && dist(x, y, menuCx(), menuCy()) <= K_SWAP_R * u() * 1.4f; }   // a hidden button is not there to be hit
    // With a controller the button would sit over the picture all the time, and the controller's own menu key opens the menu: it hides, and a touch of the screen brings it
    // back for a few seconds (it fades out, and while it is shown it is a plain button). Without a controller it is always there, as before
    static final long MENU_SHOW_MS = 3000, MENU_FADE_MS = 600;
    long menuShownAt = -MENU_SHOW_MS - MENU_FADE_MS;
    float menuAlphaAfter(long ms) { return ms < MENU_SHOW_MS ? 1 : ms < MENU_SHOW_MS + MENU_FADE_MS ? 1 - (ms - MENU_SHOW_MS) / (float)MENU_FADE_MS : 0; }
    float menuAlpha() { return !padOff || menuHeld ? 1 : menuAlphaAfter(android.os.SystemClock.uptimeMillis() - menuShownAt); }
    final Runnable menuFade = new Runnable() { public void run() { invalidate(); if (padOff && menuAlpha() > 0) postDelayed(this, 50); } };
    void menuReveal() { if (!padOff) return; menuShownAt = android.os.SystemClock.uptimeMillis(); removeCallbacks(menuFade); post(menuFade); }
    long faceAt(float x, float y) { long m = 0; float r = faceR() * 1.25f; for (int b = B_A; b <= B_Y; b++) if (dist(x, y, faceX(b), faceY(b)) <= r) m |= 1L << b; return m; }
    long dpadAt(float x, float y) {
        float dx = x - dpadCx(), dy = y - dpadCy();
        if (Math.hypot(dx, dy) < dpadR() * 0.25) return 0;
        double a = Math.toDegrees(Math.atan2(-dy, dx)); long m = 0;
        if (a > -67.5 && a < 67.5) m |= 1L << B_RIGHT;
        if (a > 22.5 && a < 157.5) m |= 1L << B_UP;
        if (a > 112.5 || a < -112.5) m |= 1L << B_LEFT;
        if (a < -22.5 && a > -157.5) m |= 1L << B_DOWN;
        return m;
    }
    float[] knob(int i) {   // the knob's offset from its stick's centre, clamped to the radius
        if (stick[i] < 0) return new float[]{0, 0};
        float dx = stickX[i] - stickCx(i), dy = stickY[i] - stickCy(i), d = (float)Math.hypot(dx, dy), r = stickR(i);
        if (d > r) { dx *= r / d; dy *= r / d; }
        return new float[]{dx, dy};
    }

    // ---- Touch ----
    void recompute() {
        long face = 0, btn = 0, dpad = 0; boolean swap = false, menu = false;
        for (int p = 0; p < 64; p++) if (down[p]) {
            int c = (int)(owner[p] & 0xff);
            if (c == C_FACE) face |= faceOf[p];
            else if (c == C_BTN) btn |= 1L << (owner[p] >> 8);
            else if (c == C_DPAD) dpad |= dpadAt(px[p], py[p]);
            else if (c == C_SWAP) swap = true;
            else if (c == C_MENU) menu = true;
        }
        int[] ax = new int[4];
        for (int i = 0; i < 2; i++) {
            float[] o = knob(i); float r = stickR(i), d = (float)Math.hypot(o[0], o[1]), dz = r * K_DEAD;
            if (d <= dz) continue;
            float m = Math.min((d - dz) / (r * K_FULL - dz), 1) / d;
            ax[2 * i] = Math.round(o[0] * m * 32767); ax[2 * i + 1] = Math.round(-o[1] * m * 32767);   // the host: up is positive
        }
        long pressed = (face | dpad | btn) & ~(faceBits | dpadBits | btnBits);
        int gripping = (stick[0] >= 0 ? 1 : 0) | (stick[1] >= 0 ? 2 : 0);
        if (pressed != 0 || (swap && !swapHeld) || (menu && !menuHeld) || (gripping & ~hapticSticks) != 0) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        hapticSticks = gripping;
        faceBits = face; dpadBits = dpad; btnBits = btn; swapHeld = swap; menuHeld = menu;
        nativeTouchPad(face | dpad | btn, ax[0], ax[1], ax[2], ax[3]);
        lastBtn = face | dpad | btn; lastAx = ax;
        invalidate();
    }
    void pushGame() {   // the game's touch screen: normalized on the picture (the centred 16:9 area of the band it is shown in: front_android.c frame_draw, picture_range)
        float w = getWidth(), h = half ? getHeight() * picH : getHeight(); if (w <= 0 || h <= 0) return;
        if (w / h > 16f / 9) w = h * 16 / 9; else h = w * 9 / 16;
        float x0 = (getWidth() - w) / 2, y0 = (half ? getHeight() * picTop : 0) + (getHeight() * (half ? picH : 1) - h) / 2;
        int[] ids = new int[16]; float[] xs = new float[16], ys = new float[16]; int n = 0;
        for (int i = 0; i < 16; i++) if (game[i] >= 0) {
            ids[n] = i; xs[n] = Math.min(Math.max((px[game[i]] - x0) / w, 0), 0.999f); ys[n] = Math.min(Math.max((py[game[i]] - y0) / h, 0), 0.999f); n++;
        }
        nativeGameTouch(ids, xs, ys, n); if (n > 0) gameTouches++;
    }
    boolean gameBegin(int p) { for (int i = 0; i < 16; i++) if (game[i] < 0) { game[i] = p; owner[p] = C_GAME | (long)i << 8; return true; } return false; }
    void begin(int p, float x, float y) {
        down[p] = true; px[p] = x; py[p] = y; owner[p] = C_NONE; faceOf[p] = 0;
        boolean g = false, inPic = half && y < padTop;   // the band's controls are laid out below padTop: a touch above it is the picture's
        boolean menuThere = !inPic && inMenu(x, y); menuReveal();   // judged before the touch brings the button back: a tap on its hidden place goes to the game
        if (menuThere) owner[p] = C_MENU;   // shown with a controller too (the pad hidden): the way back to the launcher
        else if (padOff) g = gameBegin(p);
        else if (inPic) g = gameBegin(p);
        else {
            float bx = x, by = half ? y - padTop : y;   // into the band (box): the controls' own coordinates
            int hit = tap ? -1 : btnAt(bx, by);
            if (hit >= 0) owner[p] = C_BTN | (long)hit << 8;
            else if (inSwap(bx, by)) owner[p] = C_SWAP;
            else if (tap) g = gameBegin(p);
            else {   // the diamond, the D-pad, the left stick: the smallest distance / grab radius, if it is <= 1
                float best = 1; int kind = C_NONE, idx = 0;
                float f = dist(bx, by, faceCx(), faceCy()) / (faceGap() + faceR() * (full ? K_FACE_GRAB_3D : K_FACE_GRAB)); if (f <= best) { best = f; kind = C_FACE; }
                if (full) { float d = dist(bx, by, dpadCx(), dpadCy()) / (dpadR() * K_DPAD_GRAB); if (d < best) { best = d; kind = C_DPAD; } }
                if (stick[0] < 0) { float s = dist(bx, by, homeX(), homeY()) / (stickR(0) * K_STICK_GRAB); if (s < best) { best = s; kind = C_STICK; } }
                if (kind == C_NONE && full && stick[1] < 0 && inRightArea(x)) { kind = C_STICK; idx = 1; rcx = bx; rcy = by; }
                if (kind == C_FACE) { owner[p] = C_FACE; faceOf[p] = faceAt(bx, by); }
                else if (kind == C_DPAD) owner[p] = C_DPAD;
                else if (kind == C_STICK) { owner[p] = C_STICK | (long)idx << 8; stick[idx] = p; stickX[idx] = bx; stickY[idx] = by; }
                else g = gameBegin(p);
            }
        }
        if (g) pushGame();
    }
    void move(int p, float x, float y) {
        if (!down[p]) return;
        px[p] = x; py[p] = y; int c = (int)(owner[p] & 0xff);
        if (c == C_FACE) faceOf[p] = faceAt(x, half ? y - padTop : y);
        else if (c == C_STICK) { int i = (int)(owner[p] >> 8); float bx = x, by = half ? y - padTop : y; stickX[i] = bx; stickY[i] = by;
            if (i == 1) { float d = dist(bx, by, rcx, rcy), r = stickR(1); if (d > r) { rcx = bx - (bx - rcx) * r / d; rcy = by - (by - rcy) * r / d; } } }
    }
    boolean end(int p, boolean ended) {   // → the layout button was released on it
        if (!down[p]) return false;
        boolean swap = ended && (owner[p] & 0xff) == C_SWAP && inSwap(px[p], half ? py[p] - padTop : py[p]);
        if (ended && (owner[p] & 0xff) == C_MENU && inMenu(px[p], half ? py[p] - padTop : py[p])) menuGo = true;   // released on it
        if ((owner[p] & 0xff) == C_GAME) game[(int)(owner[p] >> 8) & 15] = -1;
        for (int i = 0; i < 2; i++) if (stick[i] == p) stick[i] = -1;
        down[p] = false; owner[p] = 0; faceOf[p] = 0;
        return swap;
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        if (menuOpen) return menuTouch(e);
        int a = e.getActionMasked(), ai = e.getActionIndex(); boolean swap = false, gameMoved = false;
        switch (a) {
            case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: { int p = e.getPointerId(ai) & 63; begin(p, e.getX(ai), e.getY(ai)); break; }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) { int p = e.getPointerId(i) & 63; move(p, e.getX(i), e.getY(i)); if ((owner[p] & 0xff) == C_GAME) gameMoved = true; }
                break;
            case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: { int p = e.getPointerId(ai) & 63; boolean g = (owner[p] & 0xff) == C_GAME; swap = end(p, true); if (g) pushGame(); break; }
            case MotionEvent.ACTION_CANCEL: for (int p = 0; p < 64; p++) end(p, false); pushGame(); break;
        }
        if (gameMoved) pushGame();
        recompute();
        if (swap) nextLayout();
        if (menuGo) { menuGo = false; menuSet(true); }
        return true;
    }
    void nextLayout() {   // 2D → 3D (→ Tap in handheld mode) → 2D; every touch released first
        int l = tap ? 0 : full ? (nativeOpMode() == 0 ? 2 : 0) : 1;
        reset();
        full = l == 1; tap = l == 2;
        prefs.edit().putInt("DmPadLayout", l).apply();
        invalidate();
    }
    void reset() { for (int p = 0; p < 64; p++) end(p, false); java.util.Arrays.fill(game, -1); pushGame(); recompute(); }

    // ---- Drawing ----
    void shape(Canvas c, Path p, boolean on) { fill.setColor(on ? ON : IDLE); c.drawPath(p, fill); c.drawPath(p, line); }
    void label(Canvas c, String s, float x, float y, float size, boolean on) {
        if (s == null) return;
        text.setTextSize(size); text.setColor(on ? TEXT_ON : TEXT);
        Paint.FontMetrics fm = text.getFontMetrics(); c.drawText(s, x, y - (fm.ascent + fm.descent) / 2, text);
    }
    static Path circle(float x, float y, float r) { Path p = new Path(); p.addCircle(x, y, r, Path.Direction.CW); return p; }
    static Path drop(float cx, float cy, float r, double tip) {   // a teardrop: circle and two tangents meeting at the tip
        double d = r * K_DROP_TIP, th = Math.acos(r / d);
        Path p = new Path();
        p.moveTo((float)(cx + r * Math.cos(tip - th)), (float)(cy + r * Math.sin(tip - th)));
        p.lineTo((float)(cx + d * Math.cos(tip)), (float)(cy + d * Math.sin(tip)));
        p.lineTo((float)(cx + r * Math.cos(tip + th)), (float)(cy + r * Math.sin(tip + th)));
        p.arcTo(new RectF(cx - r, cy - r, cx + r, cy + r), (float)Math.toDegrees(tip + th), (float)(360 - Math.toDegrees(2 * th)));
        p.close(); return p;
    }
    static Path arm(float cx, float cy, float r, float w, float in, double angle) {   // a D-pad arm: an upward pentagon (tip toward the centre), rotated
        float[][] v = {{-w, -r}, {w, -r}, {w, -(in + w)}, {0, -in}, {-w, -(in + w)}};
        double cs = Math.cos(angle), sn = Math.sin(angle); Path p = new Path();
        for (int i = 0; i < 5; i++) { float x = (float)(cx + v[i][0] * cs - v[i][1] * sn), y = (float)(cy + v[i][0] * sn + v[i][1] * cs); if (i == 0) p.moveTo(x, y); else p.lineTo(x, y); }
        p.close(); return p;
    }
    // ---- Frame-rate counter (DM_SHOW_FPS=1, the settings screen's Frame-rate counter): the game's presented frames over each half second, at the top left ----
    String fps; long fpsFrames, fpsAt; Paint fpsPaint; boolean fpsOn;
    final Handler fpsH = new Handler(Looper.getMainLooper()); final Runnable fpsRun = new Runnable() { public void run() { fpsTick(); } };
    void fpsTick() {
        long n = nativeFrames(), t = android.os.SystemClock.elapsedRealtime();
        if (fpsAt != 0 && t > fpsAt) { fps = String.format(java.util.Locale.ROOT, "%.0f fps", (n - fpsFrames) * 1000.0 / (t - fpsAt)); invalidate(); }
        fpsFrames = n; fpsAt = t;
        fpsH.postDelayed(fpsRun, 500);
    }
    void fpsSet(boolean on) {   // the launcher's choice at the start; the pause menu's row for this run of the game
        fpsH.removeCallbacks(fpsRun); fpsOn = on; fps = null; fpsAt = 0;
        if (on) fpsTick(); else invalidate();
    }
    void fpsDraw(Canvas c) {
        if (fps == null) return;
        if (fpsPaint == null) { fpsPaint = new Paint(Paint.ANTI_ALIAS_FLAG); fpsPaint.setTypeface(Typeface.DEFAULT_BOLD); fpsPaint.setShadowLayer(3, 1, 1, Color.BLACK); }
        float u = u(); fpsPaint.setTextSize(16 * u); fpsPaint.setColor(Color.argb(230, 255, 255, 255)); c.drawText(fps, 12 * u, (half ? padTop : 0) + 26 * u, fpsPaint);
    }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        layoutBand();   // the band's share of the height, now that there is one
    }

    @Override protected void onDraw(Canvas c) {
        fpsDraw(c);
        if (menuOpen) { menuDraw(c); return; }
        float u = u(); long all = faceBits | dpadBits | btnBits;
        if (!padOff) {
            if (!tap) {
                for (int b = B_A; b <= B_Y; b++) { shape(c, circle(faceX(b), faceY(b), faceR()), (all >> b & 1) != 0); label(c, NAMES[b], faceX(b), faceY(b), 20 * u, (all >> b & 1) != 0); }
                for (int b : K_BTNS) {
                    if (fullOnly(b) && !full) continue;
                    boolean on = (all >> b & 1) != 0;
                    if (isDrop(b)) { boolean r = b == B_ZR || b == B_R; shape(c, drop(dropCx(b), dropCy(b), dropR(b), r ? -Math.PI / 4 : -3 * Math.PI / 4), on); label(c, NAMES[b], dropCx(b), dropCy(b), 16 * u, on); }
                    else if (isClick(b)) { shape(c, circle(clickCx(b), clickCy(), K_CLICK_R * u), on); label(c, NAMES[b], clickCx(b), clickCy(), 13 * u, on); }
                    else { RectF r = pill(b); Path p = new Path(); p.addRoundRect(r, r.height() / 2, r.height() / 2, Path.Direction.CW); shape(c, p, on); label(c, NAMES[b], r.centerX(), r.centerY(), 18 * u, on); }
                }
                if (full) { int[] arms = {B_UP, B_RIGHT, B_DOWN, B_LEFT}; for (int i = 0; i < 4; i++) shape(c, arm(dpadCx(), dpadCy(), dpadR(), K_ARM_W * u, K_ARM_IN * u, i * Math.PI / 2), (all >> arms[i] & 1) != 0); }
                fill.setColor(RING); c.drawCircle(homeX(), homeY(), stickR(0), fill); c.drawCircle(homeX(), homeY(), stickR(0), line);
                float[] o = knob(0); float k = K_KNOB_R * u;
                shape(c, circle(homeX() + o[0], homeY() + o[1], k), stick[0] >= 0); c.drawCircle(homeX() + o[0], homeY() + o[1], k * 0.62f, line);
            }
            shape(c, circle(swapCx(), swapCy(), K_SWAP_R * u), swapHeld); label(c, tap ? "Tap" : full ? "3D" : "2D", swapCx(), swapCy(), 12 * u, swapHeld);
        }
        float ma = menuAlpha();
        if (ma > 0) {
            float r = K_SWAP_R * u * 1.3f; int sv = c.saveLayerAlpha(menuCx() - r, menuCy() - r, menuCx() + r, menuCy() + r, (int)(255 * ma));
            shape(c, circle(menuCx(), menuCy(), K_SWAP_R * u), menuHeld); label(c, "≡", menuCx(), menuCy(), 18 * u, menuHeld);
            c.restoreToCount(sv);
        }
    }

    // ---- The pause menu: ≡ (or a controller's Mode / Menu key, front_android.c) opens it and the game is held under it (nativePause) until it closes ----
    // Rows that every device and game has: nothing here is saved but the touch layout, which the layout button saves too. Touch: a row is chosen where the finger lifts
    // on the row it went down on, ≡ again resumes. A controller: up / down, A chooses, left / right change a row's value, B resumes (menuKey, from the native input queue)
    // M_SPLIT is the game above and the controls below (the layout button's own Tap layout leaves no controls to put there). Its value is written to launcher.txt and read at
    // the next start: the picture is placed when the host starts (front_android.c picture_range), so this run keeps the split it started with
    static final int M_RESUME = 0, M_LAYOUT = 1, M_SPLIT = 2, M_FPS = 3, M_SAVE = 4, M_LEAVE = 5;   // M_SAVE is the launcher's screen: a game app, which has none, has no such row
    int menuN() { return (hasLauncher() ? 6 : 5) - (tap ? 1 : 0); }
    // the row on the screen → its id: the split's row is gone in Tap, and the save's without a launcher
    int menuId(int row) {
        int i = row;
        if (i >= M_SPLIT && tap) i++;
        if (i >= M_SAVE && !hasLauncher()) i++;
        return i;
    }
    static final int MK_TOGGLE = 0, MK_UP = 1, MK_DOWN = 2, MK_LEFT = 3, MK_RIGHT = 4, MK_OK = 5, MK_BACK = 6;   // front_android.c's
    static final float K_MENU_W = 300, K_MENU_HEAD = 48, K_MENU_ROW = 46, K_MENU_PAD = 10;
    boolean menuOpen; int menuSel, menuDown = -1; Paint menuPaint;
    void menuSet(boolean open) {
        if (open == menuOpen) return;
        if (open) {
            reset();   // every touch released first
            if (!hold(true)) { toLauncher(); return; }   // no game to hold (the status screen), or a host that cannot hold one: ≡ leaves, as it did
            menuSel = M_RESUME; menuDown = -1;
        } else hold(false);
        menuOpen = open; invalidate();
    }
    // An app can carry hosts built before the menu (a game kept on its tested host): they register no nativePause, and the call throws. Such a host has no menu
    boolean hold(boolean on) {
        try { return nativePause(on); }
        catch (UnsatisfiedLinkError e) { nativeLog("gamepad: menu: this game's host is older than the pause menu"); return false; }
    }
    RectF menuBox() {
        float u = baseU(), w = Math.min(K_MENU_W * u, getWidth() * 0.9f), h = (K_MENU_HEAD + menuN() * K_MENU_ROW + K_MENU_PAD) * u, cx = getWidth() / 2f, cy = (padTop + getHeight()) / 2f;
        return new RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
    }
    RectF menuRow(int i) { RectF b = menuBox(); float u = baseU(), t = b.top + (K_MENU_HEAD + i * K_MENU_ROW) * u; return new RectF(b.left + K_MENU_PAD * u, t, b.right - K_MENU_PAD * u, t + K_MENU_ROW * u); }
    int menuRowAt(float x, float y) { for (int i = 0, n = menuN(); i < n; i++) if (menuRow(i).contains(x, y)) return i; return -1; }
    String menuLabel(int i) { i = menuId(i); return i == M_RESUME ? "Resume" : i == M_LAYOUT ? "Touch controls" : i == M_SPLIT ? "Game above, controls below" : i == M_FPS ? "Frame-rate counter" : i == M_SAVE ? "Export or import the save…" : hasLauncher() ? "Back to the launcher" : "Home screen"; }
    String menuValue(int i) { i = menuId(i); return i == M_LAYOUT ? (tap ? "Tap" : full ? "3D" : "2D") : i == M_SPLIT ? (half ? splitPct() + "%" : "Off") : i == M_FPS ? (fpsOn ? "On" : "Off") : null; }
    void menuAct(int i) {
        i = menuId(i);
        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        if (i == M_RESUME) menuSet(false);
        else if (i == M_LAYOUT) nextLayout();
        else if (i == M_SPLIT) setSplit(half ? nextSplit(splitPct()) : SPLIT_DEFAULT);   // on → 45% … → 30% → off → 50%
        else if (i == M_FPS) fpsSet(!fpsOn);
        else if (i == M_SAVE) { menuSet(false); toLauncher(true); }   // the game's screen there has the Save rows (LauncherSaves.java), which ask before closing the game: a save is never swapped under a running host
        else if (i == M_LEAVE) { menuSet(false); toLauncher(); }   // as ≡ did: the game goes on behind the launcher, and Play there comes back to it
        invalidate();
    }
    boolean menuTouch(MotionEvent e) {
        int a = e.getActionMasked(); float x = e.getX(e.getActionIndex()), y = e.getY(e.getActionIndex());
        if (a == MotionEvent.ACTION_DOWN) { menuDown = inMenu(x, y) ? -2 : menuRowAt(x, y); if (menuDown >= 0) menuSel = menuDown; invalidate(); }
        else if (a == MotionEvent.ACTION_UP) {
            int d = menuDown; menuDown = -1;
            if (d == -2 && inMenu(x, y)) { performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP); menuSet(false); }
            else if (d >= 0 && menuRowAt(x, y) == d) menuAct(d);
        } else if (a == MotionEvent.ACTION_CANCEL) menuDown = -1;
        return true;
    }
    void menuKeyUi(int k) {
        if (k == MK_TOGGLE) { menuSet(!menuOpen); return; }
        if (!menuOpen) return;
        if (k == MK_UP) menuSel = (menuSel + menuN() - 1) % menuN(); else if (k == MK_DOWN) menuSel = (menuSel + 1) % menuN();
        else if (k == MK_OK) menuAct(menuSel);
        else if (k == MK_BACK) menuSet(false);
        else if ((k == MK_LEFT || k == MK_RIGHT) && menuValue(menuSel) != null) menuAct(menuSel);
        invalidate();
    }
    public static void menuKey(final int k) { new Handler(Looper.getMainLooper()).post(new Runnable() { public void run() { if (view != null) view.menuKeyUi(k); } }); }
    void menuDraw(Canvas c) {
        float u = baseU(); RectF b = menuBox();
        if (menuPaint == null) { menuPaint = new Paint(Paint.ANTI_ALIAS_FLAG); menuPaint.setTypeface(Typeface.DEFAULT_BOLD); }
        Paint p = menuPaint;
        c.drawColor(Color.argb(140, 0, 0, 0));   // the held picture, dimmed
        p.setColor(Color.argb(238, 28, 28, 32)); c.drawRoundRect(b, 16 * u, 16 * u, p);
        p.setTextSize(15 * u); Paint.FontMetrics fm = p.getFontMetrics();
        p.setColor(Color.argb(150, 255, 255, 255)); p.setTextAlign(Paint.Align.CENTER); c.drawText("Paused", b.centerX(), b.top + K_MENU_HEAD * u / 2 - (fm.ascent + fm.descent) / 2, p);
        p.setTextSize(17 * u); fm = p.getFontMetrics();
        for (int i = 0, n = menuN(); i < n; i++) {
            RectF r = menuRow(i); float y = r.centerY() - (fm.ascent + fm.descent) / 2; String v = menuValue(i);
            if (i == menuSel) { p.setColor(Color.argb(56, 255, 255, 255)); c.drawRoundRect(r, 10 * u, 10 * u, p); }
            p.setColor(Color.argb(235, 255, 255, 255)); p.setTextAlign(Paint.Align.LEFT); c.drawText(menuLabel(i), r.left + 14 * u, y, p);
            if (v != null) { p.setColor(Color.argb(150, 255, 255, 255)); p.setTextAlign(Paint.Align.RIGHT); c.drawText(v, r.right - 14 * u, y, p); }
        }
        shape(c, circle(menuCx(), menuCy(), K_SWAP_R * u()), true); label(c, "≡", menuCx(), menuCy(), 18 * u(), true);   // ≡ again resumes
    }

    // The launcher in front (the game's process keeps running behind it): Play there resumes this game, or asks before closing it for another (Launcher.java). A game app
    // (apk/AndroidManifest.game.xml, java GameStart) has no launcher: the home screen comes in front instead, the game paused behind it, and the app's icon brings it back
    int launcher = -1;   // asked once: the menu's rows read it at every draw
    boolean hasLauncher() {   // asked of the package manager: an explicit intent's resolveActivity names its component without looking it up
        if (launcher < 0) {
            Context a = getContext();
            try { a.getPackageManager().getActivityInfo(new android.content.ComponentName(a, Launcher.class), 0); launcher = 1; }
            catch (android.content.pm.PackageManager.NameNotFoundException e) { launcher = 0; }
        }
        return launcher == 1;
    }
    void toLauncher() { toLauncher(false); }
    // game: on the running game's own screen (the launcher's "game" extra, as the desktop app's Play sends it), which has its Save rows
    void toLauncher(boolean game) {
        Context a = getContext(); android.content.Intent i = new android.content.Intent(a, Launcher.class);
        if (!hasLauncher()) {
            a.startActivity(new android.content.Intent(android.content.Intent.ACTION_MAIN).addCategory(android.content.Intent.CATEGORY_HOME)
                .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));
            nativeLog("gamepad: menu: a game app has no launcher; to the home screen"); return;
        }
        i.addFlags(android.content.Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
        String id = game ? a.getSharedPreferences("app", Context.MODE_PRIVATE).getString("running", null) : null;   // Launcher.play's note of the game it started
        if (id != null) i.putExtra("game", id);
        a.startActivity(i); nativeLog("gamepad: menu: " + (id != null ? "to the launcher's screen of " + id : "back to the launcher"));
    }

    // ---- Self-check (env.txt DM_PAD_TEST=1): touches made up and sent to this view (not to the screen), each control's result checked ----
    int checks, fails; long downT;
    MotionEvent ev(int action, int[] ids, float[][] at) {
        MotionEvent.PointerProperties[] pp = new MotionEvent.PointerProperties[ids.length]; MotionEvent.PointerCoords[] pc = new MotionEvent.PointerCoords[ids.length];
        for (int i = 0; i < ids.length; i++) { pp[i] = new MotionEvent.PointerProperties(); pp[i].id = ids[i]; pp[i].toolType = MotionEvent.TOOL_TYPE_FINGER; pc[i] = new MotionEvent.PointerCoords(); pc[i].x = at[i][0]; pc[i].y = at[i][1]; pc[i].pressure = 1; pc[i].size = 1; }
        return MotionEvent.obtain(downT, android.os.SystemClock.uptimeMillis(), action, ids.length, pp, pc, 0, 0, 1, 1, 0, 0, android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
    }
    void send(MotionEvent e) { onTouchEvent(e); e.recycle(); }
    void press(float x, float y) { downT = android.os.SystemClock.uptimeMillis(); send(ev(MotionEvent.ACTION_DOWN, new int[]{0}, new float[][]{{x, y}})); }
    void release(float x, float y) { send(ev(MotionEvent.ACTION_UP, new int[]{0}, new float[][]{{x, y}})); }
    void check(String what, boolean ok) { checks++; if (!ok) fails++; nativeLog("gamepad check: " + what + " " + (ok ? "ok" : "FAILED")); }
    void tapCheck(String what, float x, float y, long want) { press(x, y); check(what + " pressed", lastBtn == want); release(x, y); check(what + " released", lastBtn == 0); }
    void selfCheck() {
        checks = fails = 0; reset();
        if (padOff) { nativeLog("gamepad check: a controller is connected, the touch controls are hidden: not checked"); return; }
        while (!full || tap) { press(swapCx(), swapCy()); release(swapCx(), swapCy()); }   // to the 3D layout
        check("the layout button gives 3D", full && !tap);
        String[] fn = {"A", "B", "X", "Y"};
        for (int b = B_A; b <= B_Y; b++) tapCheck(fn[b], faceX(b), faceY(b), 1L << b);
        for (int b : K_BTNS) {
            float x = isDrop(b) ? dropCx(b) : isClick(b) ? clickCx(b) : pill(b).centerX(), y = isDrop(b) ? dropCy(b) : isClick(b) ? clickCy() : pill(b).centerY();
            tapCheck(NAMES[b], x, y, 1L << b);
        }
        float r = dpadR() * 0.7f;
        tapCheck("D-pad up", dpadCx(), dpadCy() - r, 1L << B_UP); tapCheck("D-pad right", dpadCx() + r, dpadCy(), 1L << B_RIGHT);
        tapCheck("D-pad down", dpadCx(), dpadCy() + r, 1L << B_DOWN); tapCheck("D-pad left", dpadCx() - r, dpadCy(), 1L << B_LEFT);
        tapCheck("D-pad up-right", dpadCx() + r * 0.7f, dpadCy() - r * 0.7f, 1L << B_UP | 1L << B_RIGHT);
        // the left stick: pushed right to the ring edge, then up; released to the centre
        press(homeX(), homeY()); send(ev(MotionEvent.ACTION_MOVE, new int[]{0}, new float[][]{{homeX() + stickR(0), homeY()}}));
        check("left stick right = +32767", lastAx[0] == 32767 && lastAx[1] == 0);
        send(ev(MotionEvent.ACTION_MOVE, new int[]{0}, new float[][]{{homeX(), homeY() - stickR(0)}}));
        check("left stick up = +32767 (up is positive)", lastAx[0] == 0 && lastAx[1] == 32767);
        release(homeX(), homeY() - stickR(0)); check("left stick released = 0", lastAx[0] == 0 && lastAx[1] == 0);
        // the right stick: a touch in the right third on no control, centred where it lands
        float rx = getWidth() - getWidth() * K_RSWIPE / 2, ry = getHeight() * 0.45f;
        press(rx, ry); send(ev(MotionEvent.ACTION_MOVE, new int[]{0}, new float[][]{{rx - stickR(1), ry}}));
        check("right stick left = -32767", lastAx[2] == -32767 && lastAx[3] == 0);
        release(rx - stickR(1), ry); check("right stick released = 0", lastAx[2] == 0 && lastAx[3] == 0);
        // beside the face buttons, past their drawn edge: the camera, not a face button (#433: right swipes there did nothing)
        float bx = faceX(B_X), by = faceY(B_X) - faceR() * 1.4f;
        press(bx, by); send(ev(MotionEvent.ACTION_MOVE, new int[]{0}, new float[][]{{bx + stickR(1), by}}));
        check("beside the face buttons: right stick right = +32767", lastBtn == 0 && lastAx[2] == 32767 && lastAx[3] == 0);
        release(bx + stickR(1), by); check("and released = 0", lastAx[2] == 0 && lastAx[3] == 0);
        // two fingers: A held while the left stick moves
        downT = android.os.SystemClock.uptimeMillis();
        send(ev(MotionEvent.ACTION_DOWN, new int[]{0}, new float[][]{{faceX(B_A), faceY(B_A)}}));
        send(ev(MotionEvent.ACTION_POINTER_DOWN | 1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT, new int[]{0, 1}, new float[][]{{faceX(B_A), faceY(B_A)}, {homeX(), homeY()}}));
        send(ev(MotionEvent.ACTION_MOVE, new int[]{0, 1}, new float[][]{{faceX(B_A), faceY(B_A)}, {homeX() + stickR(0), homeY()}}));
        check("two fingers: A held and the left stick right", lastBtn == 1L << B_A && lastAx[0] == 32767);
        send(ev(MotionEvent.ACTION_POINTER_UP, new int[]{0, 1}, new float[][]{{faceX(B_A), faceY(B_A)}, {homeX() + stickR(0), homeY()}}));
        check("first finger up: the stick stays, A released", lastBtn == 0 && lastAx[0] == 32767);
        send(ev(MotionEvent.ACTION_UP, new int[]{1}, new float[][]{{homeX() + stickR(0), homeY()}}));
        // where the layout put them, at this size and inset: inside the screen, the corner groups below the shoulder buttons and clear of each other (in 3D, the widest layout)
        check(String.format(java.util.Locale.ROOT, "size %.2f (set %.2f), inset %.0f px (set %.2f), raise %.0f px (set %.2f): every control inside the screen", padScale(), scaleSet, insetPx(), insetSet, raisePx(), raiseSet),
              homeX() - stickR(0) >= 0 && faceX(B_A) + faceR() <= getWidth() && homeY() + stickR(0) <= getHeight() && faceY(B_B) + faceR() <= getHeight()
              && dropCy(B_ZL) - dropR(B_ZL) >= 0 && dropCx(B_ZL) - dropR(B_ZL) >= 0 && dropCx(B_ZR) + dropR(B_ZR) <= getWidth());
        check("the left stick's touch area below the shoulder buttons'", homeY() - stickR(0) * K_STICK_GRAB > dropCy(B_L) + dropR(B_L) * 1.35f);
        check("the left group's touch area clear of the face buttons'", cornerL() + K_LEFT_REACH * u() < faceX(B_Y) - faceR() * 1.25f);
        // the game's touch screen: a touch in the middle, on no control
        int g = gameTouches; press(getWidth() / 2f, (padTop + getHeight()) / 2f); check("a touch in the middle goes to the game's touch screen", gameTouches > g && game[0] >= 0);
        release(getWidth() / 2f, (padTop + getHeight()) / 2f); check("and is lifted", game[0] < 0);
        // the split (DM_PAD_HALF): the controls stay in the band under the picture, and a touch over the picture reaches the game, not a control
        if (half && padTop > 0) {
            check("the split: every control below the picture", homeY() - stickR(0) >= padTop && faceY(B_X) - faceR() >= padTop && dropCy(B_ZL) - dropR(B_ZL) >= padTop);
            int h0 = gameTouches; press(getWidth() / 2f, padTop / 2f); check("the split: a touch over the picture goes to the game", gameTouches > h0 && game[0] >= 0);
            release(getWidth() / 2f, padTop / 2f); check("the split: and is lifted", game[0] < 0);
            check(String.format(java.util.Locale.ROOT, "the split: the menu row reads %d%%", splitPct()), splitPct() >= SPLIT_MIN && splitPct() <= SPLIT_MAX);
        } else nativeLog("gamepad check: no split (DM_PAD_HALF): the game above the controls is not checked");
        // the pause menu, with a game to hold (without one ≡ leaves for the launcher: not pressed here)
        if (hold(true)) {
            hold(false);
            press(menuCx(), menuCy()); release(menuCx(), menuCy()); check("≡ opens the pause menu", menuOpen);
            press(faceX(B_A), faceY(B_A)); check("no control under the menu reaches the game", lastBtn == 0); release(faceX(B_A), faceY(B_A));
            boolean f = fpsOn; RectF mr = menuRow(M_FPS);
            press(mr.centerX(), mr.centerY()); release(mr.centerX(), mr.centerY()); check("its frame-rate counter row turns the counter " + (f ? "off" : "on"), fpsOn != f && menuOpen);
            press(mr.centerX(), mr.centerY()); release(mr.centerX(), mr.centerY()); check("and back", fpsOn == f);
            menuSel = M_RESUME;   // the taps left it on the counter's row
            menuKeyUi(MK_UP); check("a controller's up goes round to the last row", menuId(menuSel) == M_LEAVE);
            check("the save row is there with a launcher only", (menuN() == 5) == hasLauncher() && (menuId(3) == M_SAVE) == hasLauncher());
            menuKeyUi(MK_DOWN); menuKeyUi(MK_OK); check("down and A on Resume close it", !menuOpen);
            menuKeyUi(MK_TOGGLE); check("a controller's Mode key opens it", menuOpen);
            menuKeyUi(MK_BACK); check("and B closes it", !menuOpen);
        } else nativeLog("gamepad check: no game is running, or its host is older than the pause menu: not checked");
        nativeLog("gamepad check: " + checks + " checks, " + fails + " failed");
        while (full || tap) { press(swapCx(), swapCy()); release(swapCx(), swapCy()); }   // back to 2D
    }
    int gameTouches;

    // ---- Physical controllers: the controls hide while one is connected ----
    static boolean anyPad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            // a game controller gets a controller number (the phone's own keys and sensors that call themselves joysticks do not)
            if (d != null && !d.isVirtual() && d.getControllerNumber() > 0 && ((d.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (d.getSources() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)) return true;
        }
        return false;
    }
    void padsChanged() { boolean any = anyPad(); if (any != padOff) { padOff = any; reset(); menuReveal(); invalidate(); nativeLog("gamepad: " + (any ? "a controller is connected: the touch controls hide" : "no controller: touch controls")); } }

    // ---- Start (front_android.c, from the native thread) ----
    static Gamepad view; static PopupWindow popup;
    public static void start(final Activity a) {
        final Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() { public void run() {
            final View decor = a.getWindow().getDecorView();
            if (decor.getWindowToken() == null) { h.postDelayed(this, 100); return; }   // the activity's window is not attached yet
            ctx = a.getApplicationContext(); view = new Gamepad(a);
            view.setPadSize(nativeEnv("DM_PAD_SCALE"), nativeEnv("DM_PAD_INSET"), nativeEnv("DM_PAD_RAISE"));   // the launcher's choices (launcher.txt) or env.txt
            view.layoutBand();   // before the first frame: a split screen has its controls below the picture from the start
            popup = new PopupWindow(view, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, false);
            popup.setTouchable(true); popup.setFocusable(false); popup.setClippingEnabled(false);
            if (android.os.Build.VERSION.SDK_INT >= 22) popup.setAttachedInDecor(false);
            popup.showAtLocation(decor, Gravity.NO_GRAVITY, 0, 0);
            InputManager im = (InputManager)a.getSystemService(Context.INPUT_SERVICE);
            im.registerInputDeviceListener(new InputManager.InputDeviceListener() {
                public void onInputDeviceAdded(int id) { view.padsChanged(); }
                public void onInputDeviceRemoved(int id) { view.padsChanged(); }
                public void onInputDeviceChanged(int id) { view.padsChanged(); }
            }, h);
            view.padsChanged();
            shakeStart(a);
            nativeLog("gamepad: touch controls shown (" + (view.tap ? "Tap" : view.full ? "3D" : "2D") + (view.scaleSet != 1 || view.insetSet != 0 || view.raiseSet != 0 ?
                String.format(java.util.Locale.ROOT, ", size %.0f%%, inset %.0f%%, height %.0f%%", view.scaleSet * 100, view.insetSet * 100, view.raiseSet * 100) : "") + ")");
            if ("1".equals(nativeEnv("DM_SHOW_FPS"))) view.fpsSet(true);
            view.nativeLog("gamepad: " + (view.half ? "the game above, the controls below at " + view.splitPct() + "%" : "the game over the whole screen"));
            String t = nativeEnv("DM_PAD_TEST"); if (t != null && !t.equals("0")) h.postDelayed(new Runnable() { public void run() { view.selfCheck(); } }, 1500);
        } });
    }
    public static void reset_() { new Handler(Looper.getMainLooper()).post(new Runnable() { public void run() { if (view != null) view.reset(); } }); }   // the app went to the background: nothing stays held

    // ---- Game vibration (haptics_android.c): the connected controller's vibrator, else the phone's; a one-shot of a second at the level, renewed by the caller while it lasts ----
    static Context ctx;
    public static void rumble(float level) {
        android.os.Vibrator v = null;
        for (int id : InputDevice.getDeviceIds()) { InputDevice d = InputDevice.getDevice(id);
            if (d != null && d.getControllerNumber() > 0 && d.getVibrator() != null && d.getVibrator().hasVibrator()) { v = d.getVibrator(); break; } }
        if (v == null && ctx != null) v = (android.os.Vibrator)ctx.getSystemService(Context.VIBRATOR_SERVICE);
        if (v == null || !v.hasVibrator()) return;
        if (level <= 0) { v.cancel(); return; }
        int amp = v.hasAmplitudeControl() ? Math.max(1, Math.min(255, Math.round(level * 255))) : android.os.VibrationEffect.DEFAULT_AMPLITUDE;
        // a game's vibration, not touch feedback: the phone's setting for touch vibration does not silence it
        v.vibrate(android.os.VibrationEffect.createOneShot(1000, amp), new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_GAME).build());
    }

    // ---- Shaking the phone ----
    static void shakeStart(Context c) {
        SensorManager sm = (SensorManager)c.getSystemService(Context.SENSOR_SERVICE);
        Sensor acc = sm == null ? null : sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        if (acc == null) { nativeLog("gamepad: no accelerometer, shaking the phone does nothing"); return; }
        String e = System.getenv("DM_SHAKE_G"); double t = 1.5; try { if (e != null && Double.parseDouble(e) > 0) t = Double.parseDouble(e); } catch (NumberFormatException x) { }
        final double thr = t;
        sm.registerListener(new SensorEventListener() {
            double gx, gy, gz, peak; long last = -1; boolean init, on;
            public void onSensorChanged(SensorEvent ev) {
                double x = ev.values[0] / 9.81, y = ev.values[1] / 9.81, z = ev.values[2] / 9.81;
                if (!init) { gx = x; gy = y; gz = z; init = true; }
                double g = Math.sqrt((x - gx) * (x - gx) + (y - gy) * (y - gy) + (z - gz) * (z - gz));
                gx += (x - gx) * 0.02; gy += (y - gy) * 0.02; gz += (z - gz) * 0.02;
                if (g > thr) { if (last < 0) peak = 0; last = ev.timestamp; peak = Math.max(peak, g); }
                boolean now = last >= 0 && ev.timestamp - last < 120000000L;
                if (!now && last >= 0) { nativeLog(String.format("gamepad: phone shaken (peak %.1f G)", peak)); last = -1; }
                if (now != on) { on = now; nativeShake(now); }
            }
            public void onAccuracyChanged(Sensor s, int a) { }
        }, acc, 16667, new Handler(Looper.getMainLooper()));
        nativeLog(String.format("gamepad: shaking the phone shakes the controller (threshold %.2f G)", thr));
    }

    // ---- The software keyboard's text box (front_text_input): shown on the UI thread, the caller blocks until it is confirmed (the text) or cancelled (null) ----
    // Text comes and goes as UTF-8 bytes: a JNI string is modified UTF-8, which has no character outside the BMP (front_android.c text_input_thread).
    // OK stays disabled while the text is shorter than minChars, as the console's keyboard keeps it: a game that set a minimum does not check the length itself.
    // The form hosts built before that look up (an app can carry one, for a game kept on its tested host): strings, no minimum
    public static String ask(Activity a, String title, String sub, String guide, String initial, int maxChars) {
        byte[] r = ask(a, bytes(title), bytes(sub), bytes(guide), bytes(initial), maxChars, 0);
        return r == null ? null : text(r);
    }
    static byte[] bytes(String s) { return s == null ? null : s.getBytes(java.nio.charset.StandardCharsets.UTF_8); }
    static String text(byte[] b) { return b == null ? "" : new String(b, java.nio.charset.StandardCharsets.UTF_8); }
    public static byte[] ask(final Activity a, byte[] title8, byte[] sub8, byte[] guide8, byte[] initial8, final int maxChars, int minChars) {
        final String title = text(title8), sub = text(sub8), guide = text(guide8), initial = text(initial8);
        final int min = maxChars > 0 ? Math.min(minChars, maxChars) : minChars;
        final String[] out = {null}; final CountDownLatch done = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() { public void run() {
            LinearLayout box = new LinearLayout(a); box.setOrientation(LinearLayout.VERTICAL); int pad = (int)(20 * a.getResources().getDisplayMetrics().density); box.setPadding(pad, pad / 2, pad, 0);
            if (sub != null && !sub.isEmpty()) { TextView t = new TextView(a); t.setText(sub); box.addView(t); }
            final EditText in = new EditText(a); in.setSingleLine(true); in.setText(initial); in.setSelection(in.getText().length());
            if (!guide.isEmpty()) in.setHint(guide);
            if (maxChars > 0) in.setFilters(new InputFilter[]{new InputFilter.LengthFilter(maxChars)});
            box.addView(in);
            // the device's own dialog look, as the launcher's: the activity's theme (the game's fullscreen NoTitleBar) drew it in the old Holo style
            AlertDialog d = new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(title == null || title.isEmpty() ? null : title).setView(box)
                .setPositiveButton(android.R.string.ok, new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface di, int w) { out[0] = in.getText().toString(); done.countDown(); } })
                .setNegativeButton(android.R.string.cancel, new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface di, int w) { done.countDown(); } })
                .setOnCancelListener(new android.content.DialogInterface.OnCancelListener() { public void onCancel(android.content.DialogInterface di) { done.countDown(); } }).create();
            d.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            d.show(); in.requestFocus();
            final android.widget.Button ok = d.getButton(AlertDialog.BUTTON_POSITIVE);   // there once the dialog is shown
            ok.setEnabled(in.length() >= min);
            in.addTextChangedListener(new android.text.TextWatcher() {
                public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
                public void onTextChanged(CharSequence s, int start, int before, int count) { }
                public void afterTextChanged(android.text.Editable s) { ok.setEnabled(s.length() >= min); }
            });
            if ("2".equals(nativeEnv("DM_TEXT_TEST"))) {   // the check of the box (front_android.c): typed and confirmed as a person would, after a second
                new Handler(Looper.getMainLooper()).postDelayed(new Runnable() { public void run() {
                    in.setText("Z"); nativeLog("text box check: minimum " + min + ", OK with 1 character " + (ok.isEnabled() ? "enabled" : "disabled"));
                    in.setText("Zelda\uD83D\uDE00"); ok.performClick();
                } }, 1000);
            }
        } });
        try { done.await(); } catch (InterruptedException e) { return null; }
        return out[0] == null ? null : out[0].getBytes(java.nio.charset.StandardCharsets.UTF_8);
    }
}
