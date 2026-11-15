// The launcher: the app's entry activity. First the games on the phone, then a game's settings screen (Android's counterpart of the iOS launcher,
// dm/ios/Sources/Launcher.swift; the player's page is docs/settings.md), then the game.
//   Games: <external files>/games/<title ID>/, copied by scripts/build_android_from_nsp.py (icon.jpg and control.nacp give the icon, name and version). The APK carries a
//   host per game packed on the computer, lib/arm64-v8a/libdmhost_<title ID>.so: a folder whose host this APK lacks is listed as needing an app update.
//   Settings: the host's settings for that game, grouped into Game and Controls. Play writes the choices to the game's launcher.txt (KEY=VALUE per line) and starts the
//   GameActivity with the game's title ID (intent extra "game", read by loader.c), which reads that file after env.txt without overriding it (front_android.c
//   env_file): a key env.txt sets is shown greyed out with its value. The choices are kept in SharedPreferences, per game. Defaults: the setting's own, or the game's
//   defaults.txt (KEY=VALUE, written by the packing script: the game language picked with --language).
//   The game runs in a process of its own (":game", AndroidManifest.xml): starting another game ends the one running, after asking.
//   Saves: internal storage, saves/<title ID> (front_android.c). The one-game installs kept theirs in internal files/save: the packing script's --migrate moves the game
//   folder into games/<title ID> and leaves a legacy_save marker there, and this launcher then backs every save up to <external files>/backups/<time>/ and copies
//   files/save into saves/<title ID> (files/save itself is left as it was). Back up saves does the same backup on demand; a folder pushed to backups/restore/ is copied
//   back into internal storage at the next start (after a backup of what is there), a game at a time. A game's settings screen also exports, imports and puts back
//   that game's save (its Save rows: LauncherSaves.java), and removes the game from the phone, keeping its save (Remove game: LauncherRemove.java, Removal.java).
package org.nxnative.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.system.Os;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public class Launcher extends Activity {
    static final String TAG = "DroidPlay";   // logcat's tag, as the host's (front_android.c); nxnative before 0.5.0
    static final String GAME_PROCESS = "org.nxnative.android:game";

    // nacp.py LANGUAGES: one bit of SupportedLanguageFlag (0x302C) per language, in this order
    static final String[][] LANGUAGES = {{"en-US", "American English"}, {"en-GB", "British English"}, {"ja", "Japanese"}, {"fr", "French"}, {"de", "German"},
        {"es-419", "Latin American Spanish"}, {"es", "Spanish"}, {"it", "Italian"}, {"nl", "Dutch"}, {"fr-CA", "Canadian French"}, {"pt", "Portuguese"}, {"ru", "Russian"},
        {"ko", "Korean"}, {"zh-Hant", "Traditional Chinese"}, {"zh-Hans", "Simplified Chinese"}, {"pt-BR", "Brazilian Portuguese"}};

    // A setting: an environment variable the host reads (dm/games/settings.tsv's columns; only those the Android host honours)
    static final class Setting {
        final String key, kind, section, label, note; String[] values, labels; float lo, hi, step; String def; String on = "1";   // on: the value a toggle writes when on
        Setting(String key, String kind, String section, String label, String note) { this.key = key; this.kind = kind; this.section = section; this.label = label; this.note = note; }
    }
    // A game folder on the phone
    static final class Game {
        String id, name, version; File dir; byte[] nacp; boolean hosted; long bytes;
    }

    File ext, games, internal; SharedPreferences app, prefs; Map<String, String> envFile, defaults;
    Game game;   // the one whose settings are shown
    final List<Setting> settings = new ArrayList<>();
    LauncherSaves saves; LauncherRemove remove;
    final Map<String, String> chosen = new HashMap<>();
    LinearLayout list; float dp;
    final Handler ui = new Handler(Looper.getMainLooper());

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        ext = getExternalFilesDir(null); if (ext == null) ext = getFilesDir();
        games = new File(ext, "games"); internal = getFilesDir(); saves = new LauncherSaves(this); remove = new LauncherRemove(this);
        new Thread(new Runnable() { public void run() { Removal.sweep(games); } }, "sweep").start();   // a game folder a removal cut short left (Removal.java)
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        dp = getResources().getDisplayMetrics().density;
        app = getSharedPreferences("app", MODE_PRIVATE);
        envFile = readKeys(new File(ext, "env.txt"));
        String note = restore(); String mig = migrate();
        if (note != null) toast(note);
        if (mig != null) toast(mig);
        showGames();
        opened(getIntent());
    }

    // The desktop app's Play (scripts/nxnative_android.py: am start ... --es game <title ID> [--ez play true]): that game's settings, or the game itself
    @Override protected void onNewIntent(Intent i) { super.onNewIntent(i); setIntent(i); opened(i); saves.incoming(); }   // the computer's am start when the launcher is in front already
    void opened(Intent i) {
        String id = i != null ? i.getStringExtra("game") : null; if (id == null) return;
        for (Game g : scan()) if (g.id.equalsIgnoreCase(id) && g.hosted) { showSettings(g); if (i.getBooleanExtra("play", false)) play(true); return; }
        toast("The game " + id + " is not on this phone, or this app has no host for it");
    }

    @Override protected void onResume() { super.onResume(); saves.incoming(); }   // a save the computer sent (LauncherSaves.incoming)
    @Override public void onBackPressed() { if (game != null) showGames(); else super.onBackPressed(); }
    @Override protected void onActivityResult(int req, int res, Intent data) { if (!saves.result(req, res, data)) super.onActivityResult(req, res, data); }

    // ---------------- The games ----------------
    List<Game> scan() {
        List<Game> out = new ArrayList<>();
        String libs = getApplicationInfo().nativeLibraryDir;
        File[] ds = games.listFiles(); if (ds == null) return out;
        Arrays.sort(ds);
        for (File d : ds) {
            if (!d.isDirectory() || !d.getName().matches("[0-9A-Fa-f]{16}")) continue;
            if (!new File(d, "code/main.fixups.tsv").exists()) continue;
            Game g = new Game(); g.id = d.getName().toUpperCase(Locale.ROOT); g.dir = d; g.nacp = readAll(new File(d, "control.nacp"));
            g.name = title(g.nacp, g.id); g.version = displayVersion(g.nacp); g.hosted = new File(libs, "libdmhost_" + g.id + ".so").exists();
            File img = new File(d, "RomFS.img"); g.bytes = img.exists() ? img.length() : romfsBytes(d);
            out.add(g);
        }
        return out;
    }

    // ---------------- The look ----------------
    // The desktop app's (scripts/nxnative_android.py): ink ground, one cream action (Play), the game's own colours washed behind its screen
    static final int INK = 0xFF0B0B0C, PANEL = 0xC7141416, LIFT = 0xFF1D1D20, SEG = 0xFF2A2A2E, LINE = 0x14FFFFFF;
    static final int TEXT = 0xFFF4F4F5, TEXT2 = 0xFFA1A1AA, CREAM = 0xFFFDF3DC, READY = 0xFF3DDC84, BUILD = 0xFFF5B82E;

    void showGames() {
        game = null;
        int dpH = (int) (getResources().getDisplayMetrics().heightPixels / dp);
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setBackgroundColor(INK); root.setPadding(px(40), px(24), px(40), px(24));
        LinearLayout bar = new LinearLayout(this); bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.addView(new Mark(this), new LinearLayout.LayoutParams(px(26), px(26)));
        TextView word = text("DroidPlay", 18, TEXT, 700); word.setLetterSpacing(-0.02f); word.setPadding(px(12), 0, 0, 0);
        bar.addView(word, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        final String running = runningGame();
        if (running != null) {
            TextView r = pill("Resume " + nameOf(running), LIFT, TEXT); r.setCompoundDrawablesRelative(dot(READY), null, null, null); r.setCompoundDrawablePadding(px(8)); r.setPadding(px(12), 0, px(16), 0);
            r.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { resume(); } });
            bar.addView(r, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, px(36)));
        }
        TextView bk = pill("Back up saves", 0, TEXT2);
        bk.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { String w = backup(); toast(w != null ? "Saves backed up to " + w : "Nothing to back up"); } });
        bar.addView(bk, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, px(36)));
        root.addView(bar);
        List<Game> gs = scan();
        if (gs.isEmpty()) {
            LinearLayout empty = new LinearLayout(this); empty.setOrientation(LinearLayout.VERTICAL); empty.setGravity(Gravity.CENTER_VERTICAL);
            TextView h = text("No games on this phone yet", 22, TEXT, 700); h.setLetterSpacing(-0.02f); empty.addView(h);
            TextView t = text("Pack one on your computer with DroidPlay: it copies the game here and updates this app.", 14, TEXT2, 400);
            t.setPadding(0, px(8), 0, px(20)); t.setLineSpacing(0, 1.3f); t.setMaxWidth(px(440)); empty.addView(t);
            TextView st = pill("Run the self test", LIFT, TEXT);
            st.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { selftest(); } });
            empty.addView(st, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, px(44)));
            root.addView(empty, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        } else {
            TextView label = caps("On this phone · " + gs.size() + (gs.size() == 1 ? " game" : " games")); label.setPadding(0, px(22), 0, px(22)); root.addView(label);
            int cover = Math.max(120, Math.min(220, dpH - 214));   // as large as the screen's height leaves
            HorizontalScrollView hs = new HorizontalScrollView(this); hs.setHorizontalScrollBarEnabled(false); hs.setClipToPadding(false);
            LinearLayout row = new LinearLayout(this);
            for (int i = 0; i < gs.size(); i++) {
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(px(cover), LinearLayout.LayoutParams.WRAP_CONTENT); if (i > 0) lp.leftMargin = px(24);
                row.addView(tile(gs.get(i), cover), lp);
            }
            hs.addView(row); root.addView(hs, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        }
        setContentView(root);
    }

    View tile(final Game g, int cover) {
        LinearLayout t = new LinearLayout(this); t.setOrientation(LinearLayout.VERTICAL);
        t.addView(art(g, 22), new LinearLayout.LayoutParams(px(cover), px(cover)));
        if (!g.hosted) t.getChildAt(0).setAlpha(0.45f);
        TextView n = text(g.name, 15, g.hosted ? TEXT : TEXT2, 500); n.setMaxLines(2); n.setEllipsize(android.text.TextUtils.TruncateAt.END); n.setPadding(0, px(12), 0, px(3));
        t.addView(n);
        String sub = g.hosted ? (g.version != null ? g.version + " · " : "") + String.format(Locale.ROOT, "%.1f GB", g.bytes / 1073741824.0) : "Update the app from your computer";
        TextView s = text(sub, 12, g.hosted ? TEXT2 : BUILD, 400); s.setFontFeatureSettings("tnum"); t.addView(s);
        press(t);
        t.setOnClickListener(new View.OnClickListener() { public void onClick(View v) {
            if (!g.hosted) {   // no settings screen to remove it from, so here
                new AlertDialog.Builder(Launcher.this, LauncherSaves.DIALOG).setTitle(g.name)
                    .setMessage("This app has no host for it yet (libdmhost_" + g.id + ".so): update the app from your computer to play it, or remove the game from this phone.")
                    .setPositiveButton("OK", null)
                    .setNeutralButton("Remove…", new DialogInterface.OnClickListener() { public void onClick(DialogInterface d, int w) { remove.ask(g); } }).show();
                return;
            }
            showSettings(g); } });
        return t;
    }
    String nameOf(String id) { for (Game g : scan()) if (g.id.equalsIgnoreCase(id)) return g.name; return id; }

    // ---------------- A game's settings ----------------
    void showSettings(Game g) {
        game = g;
        prefs = getSharedPreferences("launcher_" + g.id, MODE_PRIVATE);
        defaults = readKeys(new File(g.dir, "defaults.txt"));
        settings.clear(); chosen.clear();
        define(g.nacp);
        for (Setting s : settings) chosen.put(s.key, prefs.getString(s.key, s.def));
        // launcher.txt is written by the pause menu too (Gamepad.setSplit, which runs with no launcher): a row it holds wins over this screen's last saved choice, so pressing
        // Play does not undo a split made while playing. A row whose env.txt value is fixed is left out of that file anyway (the loop below)
        Map<String, String> saved = readKeys(new File(g.dir, "launcher.txt"));
        for (Setting s : settings) if (saved.containsKey(s.key)) { chosen.put(s.key, saved.get(s.key)); prefs.edit().putString(s.key, saved.get(s.key)).apply(); }
        setContentView(build(g));
    }

    void define(byte[] nacp) {
        Setting s = new Setting("DM_OPMODE", "choice", "Game", "Console mode", "Docked renders at the TV resolution: sharper, slower on a phone.");
        s.values = new String[] {"0", "1"}; s.labels = new String[] {"Handheld", "Docked"}; s.def = "0"; settings.add(s);
        List<String[]> langs = new ArrayList<>();
        int flags = nacp != null && nacp.length >= 0x3030 ? (nacp[0x302C] & 0xFF) | (nacp[0x302D] & 0xFF) << 8 | (nacp[0x302E] & 0xFF) << 16 | (nacp[0x302F] & 0xFF) << 24 : 0;
        for (int i = 0; i < LANGUAGES.length; i++) if ((flags >> i & 1) != 0) langs.add(LANGUAGES[i]);
        if (langs.size() > 1) {
            s = new Setting("DM_LANG", "choice", "Game", "Game language", null);
            s.values = new String[langs.size()]; s.labels = new String[langs.size()];
            for (int i = 0; i < langs.size(); i++) { s.values[i] = langs.get(i)[0]; s.labels[i] = langs.get(i)[1]; }
            s.def = defaults.containsKey("DM_LANG") ? defaults.get("DM_LANG") : (flags & 1) != 0 ? "en-US" : s.values[0]; settings.add(s);
        }
        byte[] fx = readAll(new File(game.dir, "code/main.fixups.tsv"));   // the rewriter marks a build with a render-scale stub (patch_nso.py RENDER_SCALE)
        if (fx != null && new String(fx, StandardCharsets.UTF_8).contains("# RENDER_SCALE	1")) {
            s = new Setting("DM_RENDER_SCALE", "range", "Game", "Render scale", "Lower draws the 3D scene at fewer pixels: faster, softer picture.");
            s.lo = 0.5f; s.hi = 1.0f; s.step = 0.05f; s.def = "1.0"; settings.add(s);
        }
        s = new Setting("DM_AUDIO_PACE", "choice", "Game", "Game timing",
            "Game time keeps levels timed to the music in step with the picture when the phone runs the game below its frame rate; the music can stutter.");
        s.values = new String[] {"real", "game"}; s.labels = new String[] {"Real time", "Game time"}; s.def = "real"; settings.add(s);
        s = new Setting("DM_SHOW_FPS", "toggle", "Game", "Frame-rate counter", null); s.def = "0"; settings.add(s);
        if (defaults.containsKey("DM_VK_MESH_LEAVE_OUT")) {   // a game whose profile names plants the host can leave out (its android_defaults.txt carries the key; gfx_vk_mesh.inc): Ocarina of Time's forest
            s = new Setting("DM_VK_MESH_LEAVE_OUT", "toggle", "Game", "Fewer plants (faster)",
                "Leaves out the ferns, flowers and the trees' canopies: about 3 ms of GPU time a frame (a OnePlus Pad 3 went from 29.1 to 30.0 fps in Kokiri Forest). The trees stand bare.");
            s.def = "0"; settings.add(s);
        }
        if (kgsl()) {   // any game, on a phone whose GPU clock the host can hold up (gfx_vk_floor.inc); on, the highest clock, as Ocarina of Time's Galaxy Z Fold 8 runs used
            s = new Setting("DM_VK_GPU_FLOOR", "toggle", "Game", "Keep the GPU clock up",
                "Holds the GPU at its highest clock: faster in some games on some phones, no faster on others, and slower on a warm phone, which then limits its GPU harder. The phone runs warmer and uses more battery.");
            s.on = "100"; s.def = "0"; settings.add(s);
        }
        s = new Setting("DM_PAD_ABXY", "choice", "Controls", "Controller face buttons",
            "By label for a controller that already gives its buttons Nintendo's letters (a Retroid in its Retro layout); by position for an Xbox-style controller.");
        s.values = new String[] {"position", "label"}; s.labels = new String[] {"By position", "By label"}; s.def = "position"; settings.add(s);   // front_android.c face_button
        s = new Setting("DM_RUMBLE", "toggle", "Controls", "Game rumble", null); s.def = "1"; settings.add(s);
        s = new Setting("DM_RUMBLE_GAIN", "range", "Controls", "Rumble strength", null); s.lo = 0; s.hi = 3; s.step = 0.1f; s.def = "1.0"; settings.add(s);
        // the touch controls' size and how far in from the sides they sit (Gamepad.java padScale, insetPx, raisePx)
        s = new Setting("DM_PAD_SCALE", "range", "Controls", "Touch control size", "Stops growing where the controls would meet."); s.lo = 0.6f; s.hi = 1.4f; s.step = 0.05f; s.def = "1.0"; settings.add(s);
        s = new Setting("DM_PAD_INSET", "range", "Controls", "Touch control inset",
            "Moves the stick, the D-pad and the buttons in from the sides. 0% is the standard layout, 100% brings them together in the middle.");
        s.lo = 0; s.hi = 1; s.step = 0.05f; s.def = "0"; settings.add(s);
        s = new Setting("DM_PAD_RAISE", "range", "Controls", "Touch control height",
            "Moves the stick, the D-pad and the buttons up from the bottom. 0% is the standard layout; 100% stops just below the shoulder buttons.");
        s.lo = 0; s.hi = 1; s.step = 0.05f; s.def = "0"; settings.add(s);
        // the game above and the controls below (Gamepad.layoutBand, front_android.c picture_range): a foldable held open, or a tablet. The split is the picture's share of
        // the height; the pause menu's row changes it while playing, and both write it to launcher.txt, from where picture_range derives the picture's top and height
        s = new Setting("DM_PAD_HALF", "toggle", "Controls", "Game above, controls below",
            "Splits the screen as a handheld does: the game in the top part, the touch controls in the band under it. For a phone held open or a tablet. The game's own touch screen shrinks to its part too.");
        s.def = "0"; settings.add(s);
        s = new Setting("DM_SPLIT", "choice", "Controls", "Split at",
            "Where the game's part ends: 50% is half the screen each.");
        s.values = new String[] {"30", "35", "40", "45", "50", "55", "60", "65", "70"};
        s.labels = new String[] {"30%", "35%", "40%", "45%", "50%", "55%", "60%", "65%", "70%"}; s.def = "50"; settings.add(s);
        // how the picture fills its part (front_android.c frame_draw). A split's half is not 16:9 -- 1840x1104 against the picture's 16:9 -- so on its own the split covers that
        // half at the aspect kept, its edges cut off: the whole half shows a picture, as the handheld the layout copies does. Crop, Keep the shape (bars above and below) and
        // Fill (the whole picture stretched) hold it there whatever the split does
        s = new Setting("DM_PICTURE_FILL", "choice", "Controls", "Picture in its part",
            "Automatic covers a split's part at the picture's shape, its edges cut off; Keep the shape shows the whole picture with bars; Fill stretches the whole picture over the part.");
        s.values = new String[] {"auto", "crop", "0", "1"}; s.labels = new String[] {"Automatic", "Crop", "Keep the shape", "Fill"}; s.def = "auto"; settings.add(s);
        for (Setting t : settings) if (defaults.containsKey(t.key)) t.def = defaults.get(t.key);
    }

    // Qualcomm's kernel GPU driver (KGSL), the one whose clock floor the host can ask for: its device node, as vk_driver.c adreno() tests it (there, or there but not
    // readable). A Mali, Xclipse or PowerVR GPU has none, and the setting would do nothing there
    static boolean kgsl() {
        try { Os.stat("/dev/kgsl-3d0"); return true; } catch (android.system.ErrnoException e) { return e.errno != android.system.OsConstants.ENOENT; }
    }

    View build(Game g) {
        android.widget.FrameLayout root = new android.widget.FrameLayout(this); root.setBackgroundColor(INK);
        // the game's colours washed over the ground: its icon shrunk to a few pixels and spread out (the desktop app's wash), fading into ink on the right
        Bitmap bm = BitmapFactory.decodeFile(new File(g.dir, "icon.jpg").getPath());
        if (bm != null) {
            ImageView wash = new ImageView(this); android.graphics.drawable.BitmapDrawable bd = new android.graphics.drawable.BitmapDrawable(getResources(), Bitmap.createScaledBitmap(bm, 12, 12, true));
            bd.setFilterBitmap(true); wash.setImageDrawable(bd); wash.setScaleType(ImageView.ScaleType.FIT_XY); wash.setAlpha(0.32f);
            root.addView(wash, new android.widget.FrameLayout.LayoutParams(-1, -1));
            View fade = new View(this); fade.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] {0x000B0B0C, 0xB30B0B0C, INK}));
            root.addView(fade, new android.widget.FrameLayout.LayoutParams(-1, -1));
        }
        LinearLayout body = new LinearLayout(this); body.setPadding(px(40), px(24), px(28), px(24));
        // left: the game, Play, back to the games
        LinearLayout left = new LinearLayout(this); left.setOrientation(LinearLayout.VERTICAL); left.setGravity(Gravity.CENTER_VERTICAL);
        left.addView(art(g, 20), new LinearLayout.LayoutParams(px(112), px(112)));
        TextView name = text(g.name, 26, TEXT, 700); name.setLetterSpacing(-0.02f); name.setLineSpacing(0, 1.05f); name.setPadding(0, px(16), 0, px(6)); left.addView(name);
        TextView meta = text((g.version != null ? g.version + " · " : "") + String.format(Locale.ROOT, "%.1f GB", g.bytes / 1073741824.0), 12, TEXT2, 400); meta.setFontFeatureSettings("tnum"); left.addView(meta);
        TextView play = pill("Play", CREAM, INK); play.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16); play.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        play.setCompoundDrawablesRelative(glyph(new float[] {4.5f, 2.5f, 13, 8, 4.5f, 13.5f}, INK, true), null, null, null); play.setCompoundDrawablePadding(px(10));
        play.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { play(); } }); press(play);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(-1, px(52)); plp.topMargin = px(16); left.addView(play, plp);
        TextView back = text("All games", 13, TEXT2, 500); back.setGravity(Gravity.CENTER_VERTICAL);
        back.setCompoundDrawablesRelative(glyph(new float[] {10, 3, 5, 8, 10, 13}, TEXT2, false), null, null, null); back.setCompoundDrawablePadding(px(6));
        back.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { showGames(); } });
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(-2, px(44)); blp.topMargin = px(4); left.addView(back, blp);
        body.addView(left, new LinearLayout.LayoutParams(px(240), -1));
        // right: the settings, on a panel that scrolls
        ScrollView sv = new ScrollView(this); sv.setVerticalScrollBarEnabled(false); sv.setBackground(round(PANEL, 22, LINE)); sv.setClipToOutline(true);
        list = new LinearLayout(this); list.setOrientation(LinearLayout.VERTICAL); list.setPadding(px(24), px(14), px(24), px(18));
        sv.addView(list); fill();
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(0, -1, 1); slp.leftMargin = px(32); body.addView(sv, slp);
        root.addView(body, new android.widget.FrameLayout.LayoutParams(-1, -1));
        return root;
    }

    void fill() {
        list.removeAllViews();
        String section = null;
        for (Setting s : settings) {
            if (!s.section.equals(section)) { section = s.section; TextView h = caps(section); h.setPadding(0, list.getChildCount() == 0 ? px(4) : px(18), 0, px(6)); list.addView(h); }
            list.addView(row(s));
        }
        TextView reset = text("Reset to tested defaults", 13, TEXT2, 500); reset.setGravity(Gravity.CENTER_VERTICAL);
        reset.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { prefs.edit().clear().apply(); for (Setting s : settings) chosen.put(s.key, s.def); fill(); } });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, px(44)); lp.topMargin = px(12);
        list.addView(reset, lp);
        list.addView(saves.rows(game));
        list.addView(remove.rows(game));
    }

    View row(final Setting s) {
        final boolean fixed = envFile.containsKey(s.key); final String cur = fixed ? envFile.get(s.key) : chosen.get(s.key);
        LinearLayout r = new LinearLayout(this); r.setGravity(Gravity.CENTER_VERTICAL); r.setMinimumHeight(px(48)); r.setPadding(0, px(6), 0, px(6));
        LinearLayout words = new LinearLayout(this); words.setOrientation(LinearLayout.VERTICAL);
        words.addView(text(s.label, 14, TEXT, 400));
        String note = fixed ? "Set in env.txt: " + s.key + "=" + cur : s.note;
        if (note != null) { TextView t = text(note, 11, TEXT2, 400); t.setPadding(0, px(2), px(12), 0); t.setLineSpacing(0, 1.2f); words.addView(t); }
        r.addView(words, new LinearLayout.LayoutParams(0, -2, 1));
        View ctl;
        if (s.kind.equals("choice") && s.values.length <= 3) {   // a segmented control: every choice in sight
            final LinearLayout seg = new LinearLayout(this); seg.setPadding(px(3), px(3), px(3), px(3)); seg.setBackground(round(INK, 18, 0x0FFFFFFF));
            int at = Math.max(indexOf(s.values, cur), 0);
            for (int i = 0; i < s.values.length; i++) {
                final int k = i; TextView b = text(s.labels[i], 12, i == at ? TEXT : TEXT2, 500); b.setGravity(Gravity.CENTER); b.setPadding(px(14), 0, px(14), 0);
                if (i == at) b.setBackground(round(SEG, 15, 0));
                b.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { set(s, s.values[k]); fill(); } });
                seg.addView(b, new LinearLayout.LayoutParams(-2, px(30)));
            }
            ctl = seg;
        } else if (s.kind.equals("choice")) {   // a longer list: the value, and a sheet to pick from
            int at = Math.max(indexOf(s.values, cur), 0);
            TextView v = text(s.labels[at], 13, TEXT2, 500); v.setGravity(Gravity.CENTER_VERTICAL); v.setPadding(px(12), 0, 0, 0);
            v.setCompoundDrawablesRelative(null, null, glyph(new float[] {6, 3, 11, 8, 6, 13}, TEXT2, false), null); v.setCompoundDrawablePadding(px(4));
            v.setOnClickListener(new View.OnClickListener() { public void onClick(View x) {
                new AlertDialog.Builder(Launcher.this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(s.label)
                    .setSingleChoiceItems(s.labels, Math.max(indexOf(s.values, chosen.get(s.key)), 0), new DialogInterface.OnClickListener() {
                        public void onClick(DialogInterface d, int i) { set(s, s.values[i]); d.dismiss(); fill(); } }).show(); } });
            ctl = v; v.setMinHeight(px(44));
        } else if (s.kind.equals("toggle")) {
            Switch sw = new Switch(this); sw.setChecked(!"0".equals(cur));
            int[][] st = {{android.R.attr.state_checked}, {}};
            sw.setThumbTintList(new android.content.res.ColorStateList(st, new int[] {INK, TEXT2}));
            sw.setTrackTintList(new android.content.res.ColorStateList(st, new int[] {CREAM, SEG})); sw.setTrackTintMode(android.graphics.PorterDuff.Mode.SRC);
            sw.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() { public void onCheckedChanged(android.widget.CompoundButton v, boolean on) { set(s, on ? s.on : "0"); } });
            ctl = sw;
        } else {
            LinearLayout box = new LinearLayout(this); box.setGravity(Gravity.CENTER_VERTICAL);
            final TextView val = text("", 13, TEXT2, 400); val.setMinWidth(px(44)); val.setGravity(Gravity.END); val.setFontFeatureSettings("tnum");
            SeekBar sb = new SeekBar(this); final int n = Math.round((s.hi - s.lo) / s.step); sb.setMax(n);
            sb.setProgressTintList(android.content.res.ColorStateList.valueOf(CREAM)); sb.setThumbTintList(android.content.res.ColorStateList.valueOf(CREAM));
            sb.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(SEG)); sb.setProgressBackgroundTintMode(android.graphics.PorterDuff.Mode.SRC);
            float f = parse(cur, Float.parseFloat(s.def)); sb.setProgress(Math.round((f - s.lo) / s.step)); val.setText(pct(f));
            sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                public void onProgressChanged(SeekBar b, int p, boolean user) { float v = s.lo + p * s.step; val.setText(pct(v)); if (user) set(s, String.format(Locale.ROOT, "%.2f", v)); }
                public void onStartTrackingTouch(SeekBar b) {}
                public void onStopTrackingTouch(SeekBar b) {}
            });
            box.addView(sb, new LinearLayout.LayoutParams(px(160), -2)); box.addView(val);
            ctl = box;
        }
        ctl.setEnabled(!fixed);
        if (fixed) { ctl.setAlpha(0.45f); if (ctl instanceof LinearLayout) for (int i = 0; i < ((LinearLayout) ctl).getChildCount(); i++) { ((LinearLayout) ctl).getChildAt(i).setEnabled(false); ((LinearLayout) ctl).getChildAt(i).setClickable(false); } ctl.setClickable(false); }
        r.addView(ctl);
        return r;
    }

    void set(Setting s, String v) { chosen.put(s.key, v); prefs.edit().putString(s.key, v).apply(); }

    // ---------------- Starting a game ----------------
    void play() { play(false); }
    // desktop: the desktop app's Play started it (it said the first start takes a while before it did), so a first start gets a toast here, not a dialog to tap
    void play(final boolean desktop) {
        final Game g = game;
        if (saves.busy) { toast("Wait for the save to finish copying, then press Play"); return; }
        StringBuilder sb = new StringBuilder("# written by the settings screen (Launcher.java); env.txt wins over these\n");
        // A choice left empty is the setting's own default, which the host applies when the row is absent: writing "KEY=" would override that with an empty value (a
        // picture row's Automatic is the split's own choice, not a value to pass on)
        for (Setting s : settings) { String v = chosen.get(s.key); if (!envFile.containsKey(s.key) && v != null && !v.isEmpty()) sb.append(s.key).append('=').append(v).append('\n'); }
        try (FileOutputStream o = new FileOutputStream(new File(g.dir, "launcher.txt"))) { o.write(sb.toString().getBytes(StandardCharsets.UTF_8)); } catch (IOException e) { Log.w(TAG, "launcher.txt: " + e); }
        final String running = runningGame();
        if (running != null && running.equals(g.id)) { resume(); return; }
        Runnable go = new Runnable() { public void run() {
            if (running == null) { start(g.id, false); return; }
            new AlertDialog.Builder(Launcher.this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("Switch games?")
                .setMessage("Another game is running. Starting " + g.name + " closes it: progress it has not saved is lost.")
                .setPositiveButton("Close it and start", new DialogInterface.OnClickListener() { public void onClick(DialogInterface d, int w) { endGame(); later(g.id, false); } })
                .setNegativeButton("Cancel", null).show();
        } };
        if (!firstStart(g.id)) { go.run(); return; }
        // A first start stays on the loading screen while the driver builds the game's pipelines: 80 seconds for Brilliant Diamond on a Galaxy S25 Edge (2,978
        // pipelines and 8,844 shader libraries), 39 on a OnePlus Pad 3, a few seconds the next time. A player who isn't told takes it for a crash
        Log.i(TAG, "launcher: first start of " + g.id + " (no pipeline cache)");
        if (desktop) { toast(g.name + " prepares its shaders for its first start: it stays on the loading screen for a minute or two"); go.run(); return; }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("The first start takes a minute or two")
            .setMessage(g.name + " prepares its shaders first, and stays on the loading screen meanwhile. The phone may get warm. Later starts take a few seconds.")
            .setPositiveButton("Start", new DialogInterface.OnClickListener() { public void onClick(DialogInterface d, int w) { go.run(); } })
            .setNegativeButton("Cancel", null).show();
    }
    // No driver pipeline cache of the game's in internal storage: cache/<id>/pipeline.cache, where front_android.c points DM_PIPELINE_CACHE, or with a custom Vulkan
    // driver's name after it (gfx_vk_draw.inc). pipeline.cache.keys4 beside it is the list of pipelines to make again, which a start without the driver's cache
    // still makes slowly
    boolean firstStart(String id) {
        File[] fs = new File(internal, "cache/" + id.toUpperCase(Locale.ROOT)).listFiles();
        if (fs != null) for (File f : fs) { String n = f.getName(); if (n.startsWith("pipeline.cache") && !n.contains(".keys") && f.length() > 0) return false; }
        return true;
    }
    void selftest() {
        List<String> hosts = hosts(); if (hosts.isEmpty()) { toast("This app carries no game host"); return; }
        if (runningGame() != null) endGame();
        later(hosts.get(0), true);
    }
    void later(final String id, final boolean selftest) { ui.postDelayed(new Runnable() { public void run() { start(id, selftest); } }, 600); }   // after the old process is gone
    void start(String id, boolean selftest) {
        app.edit().putString("running", id).apply();
        Intent i = new Intent(this, GameActivity.class); i.putExtra("game", id); i.putExtra("selftest", selftest);
        startActivity(i); overridePendingTransition(0, 0);
    }
    void resume() {
        Intent i = new Intent(this, GameActivity.class); i.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        startActivity(i); overridePendingTransition(0, 0);
    }
    ActivityManager.RunningAppProcessInfo gameProcess() {
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE); if (am == null) return null;
        List<ActivityManager.RunningAppProcessInfo> ps = am.getRunningAppProcesses(); if (ps == null) return null;
        for (ActivityManager.RunningAppProcessInfo p : ps) if (GAME_PROCESS.equals(p.processName)) return p;
        return null;
    }
    String runningGame() { return gameProcess() != null ? app.getString("running", null) : null; }
    // The game's task goes too: a singleTask activity whose process was killed is otherwise recreated from its first intent on the next start (the closed game's
    // "game" extra; the new one only reaches onNewIntent, which the game activity ignores), and the game that was closed opened again
    void endGame() {
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (am != null) for (ActivityManager.AppTask t : am.getAppTasks()) {
            ComponentName b = t.getTaskInfo().baseActivity;
            if (b != null && GameActivity.class.getName().equals(b.getClassName())) t.finishAndRemoveTask();
        }
        ActivityManager.RunningAppProcessInfo p = gameProcess(); if (p != null) android.os.Process.killProcess(p.pid);
    }
    List<String> hosts() {   // the games this APK has a host for
        List<String> out = new ArrayList<>(); File[] fs = new File(getApplicationInfo().nativeLibraryDir).listFiles(); if (fs == null) return out;
        for (File f : fs) { String n = f.getName(); if (n.startsWith("libdmhost_") && n.endsWith(".so")) out.add(n.substring(10, n.length() - 3)); }
        return out;
    }

    // ---------------- Saves: backup, restore, the one-game installs' saves ----------------
    // Every save of the app's internal storage (the one-game installs' save, save_device and their .work copies; saves/) to <external files>/backups/<time>/, which adb
    // can pull. Files keep their xattrs (hle_fs.c's guest names of case collisions) in xattrs.tsv there: external storage does not keep them. → the folder, or null
    String backup() {
        File[] fs = internal.listFiles(); if (fs == null) return null;
        List<File> src = new ArrayList<>();
        for (File f : fs) if (f.getName().startsWith("save")) src.add(f);   // save, save_device, save.work, saves
        if (src.isEmpty()) return null;
        File to = new File(ext, "backups/" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()));
        StringBuilder xa = new StringBuilder();
        try {
            for (File f : src) copyTree(f, new File(to, f.getName()), xa, f.getName());
            writeText(new File(to, "xattrs.tsv"), xa.toString());
            writeText(new File(to, "README.txt"), "DroidPlay save backup: the app's internal save folders as they were. Copy this folder to backups/restore/ on the phone to put them back.\n");
        } catch (IOException e) { Log.w(TAG, "backup: " + e); toast("Backup failed: " + e.getMessage()); return null; }
        Log.i(TAG, "saves backed up to " + to);
        return to.getPath();
    }
    // A folder pushed to <external files>/backups/restore/ (a backup) goes back into internal storage, over the folders of the same names, after a backup of those.
    // saves/ goes back a game at a time, so a game the backup has no save of keeps its own (a one-game backup from an import or an export holds saves/<title ID> alone)
    String restore() {
        File r = new File(ext, "backups/restore"); if (!r.isDirectory()) return null;
        if (gameProcess() != null) return "A game is running: the backup in backups/restore is restored at the next start";
        String before = backup();
        Map<String, String[]> xa = readXattrs(new File(r, "xattrs.tsv"));
        File[] fs = r.listFiles(); if (fs == null) return null;
        try {
            for (File f : fs) {
                if (!f.isDirectory() || !f.getName().startsWith("save")) continue;
                if (!f.getName().equals("saves")) { restoreTree(f, new File(internal, f.getName()), f.getName(), xa); continue; }
                File[] gs = f.listFiles(); if (gs != null) for (File g : gs)
                    if (g.isDirectory() && Saves.TITLE.matcher(g.getName()).matches()) restoreTree(g, new File(internal, "saves/" + g.getName()), "saves/" + g.getName(), xa);
            }
        } catch (IOException e) { Log.w(TAG, "restore: " + e); return "Restore failed: " + e.getMessage(); }
        r.renameTo(new File(ext, "backups/restored-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date())));
        return "Saves restored" + (before != null ? " (the ones before are in " + before + ")" : "");
    }
    static void restoreTree(File src, File dst, String top, Map<String, String[]> xa) throws IOException {
        File tmp = new File(dst.getPath() + ".restoring");
        deleteTree(tmp); dst.getParentFile().mkdirs(); copyTree(src, tmp, null, null); applyXattrs(tmp, top, xa);
        deleteTree(dst); if (!tmp.renameTo(dst)) throw new IOException("rename " + tmp + " failed");
    }
    // The one-game installs kept their save in internal files/save (and save_device beside it); --migrate leaves games/<id>/legacy_save on the game they belonged to.
    // That save is copied to saves/<id> (through a temporary folder, so a copy cut short is made again next time), after a backup of everything; files/save stays as it was
    String migrate() {
        File[] ds = games.listFiles(); if (ds == null) return null;
        for (File d : ds) {
            File mark = new File(d, "legacy_save"); if (!mark.exists() || !Removal.ID.matcher(d.getName()).matches()) continue;   // not a folder being removed
            String id = d.getName().toUpperCase(Locale.ROOT);
            File old = new File(internal, "save"), dst = new File(internal, "saves/" + id);
            if (!old.isDirectory() || hasFiles(dst)) { mark.renameTo(new File(d, "legacy_save.done")); continue; }   // nothing to copy, or saves/<id> holds a save already
            File dev = new File(dst.getPath() + "_device");
            if (hasFiles(dev)) { mark.renameTo(new File(d, "legacy_save.done")); continue; }   // a device save there already: leave both as they are
            deleteTree(dst); deleteTree(dev);   // empty folders a start before the copy made
            String bk = backup();
            if (bk == null) return "The save could not be backed up, so it was not moved yet: free some space and open the app again";
            try {
                for (String n : new String[] {"save", "save_device"}) {
                    File s = new File(internal, n); if (!s.isDirectory()) continue;
                    File t = new File(internal, "saves/" + id + (n.equals("save") ? "" : "_device")), tmp = new File(t.getPath() + ".copying");
                    deleteTree(tmp); copyTree(s, tmp, null, null); copyXattrsTree(s, tmp);
                    if (!tmp.renameTo(t)) throw new IOException("rename " + tmp + " failed");
                }
                File pc = new File(internal, "pipeline.cache"), pcd = new File(internal, "cache/" + id + "/pipeline.cache");
                if (pc.isFile() && !pcd.exists()) { pcd.getParentFile().mkdirs(); copyFile(pc, pcd); }
                SharedPreferences was = getSharedPreferences("launcher", MODE_PRIVATE), now = getSharedPreferences("launcher_" + id, MODE_PRIVATE);
                if (now.getAll().isEmpty() && !was.getAll().isEmpty()) { SharedPreferences.Editor e = now.edit(); for (Map.Entry<String, ?> en : was.getAll().entrySet()) e.putString(en.getKey(), String.valueOf(en.getValue())); e.apply(); }
            } catch (IOException e) { Log.w(TAG, "migrate: " + e); return "The save could not be copied (" + e.getMessage() + "): it is untouched in the old folder and in " + bk; }
            mark.renameTo(new File(d, "legacy_save.done"));
            Log.i(TAG, "save of the one-game install copied to saves/" + id + " (backup in " + bk + "; files/save left as it was)");
            return "Your save was copied for " + title(readAll(new File(d, "control.nacp")), id) + " (a backup is in " + bk + ")";
        }
        return null;
    }

    // ---- file helpers ----
    static void copyTree(File s, File d, StringBuilder xa, String rel) throws IOException {
        if (s.isDirectory()) {
            if (!d.isDirectory() && !d.mkdirs()) throw new IOException("mkdir " + d);
            if (xa != null) xattrsOf(s, rel, xa);
            String[] ns = s.list(); if (ns == null) throw new IOException("list " + s);
            for (String n : ns) copyTree(new File(s, n), new File(d, n), xa, rel + "/" + n);
        } else { copyFile(s, d); if (xa != null) xattrsOf(s, rel, xa); }
    }
    static void copyFile(File s, File d) throws IOException {
        try (FileInputStream in = new FileInputStream(s); FileOutputStream out = new FileOutputStream(d)) {
            byte[] b = new byte[1 << 16]; int k; while ((k = in.read(b)) > 0) out.write(b, 0, k); out.getFD().sync();
        }
        d.setLastModified(s.lastModified());
    }
    static boolean hasFiles(File f) { if (f.isFile()) return true; File[] ks = f.listFiles(); if (ks != null) for (File k : ks) if (hasFiles(k)) return true; return false; }
    static void deleteTree(File f) { File[] ks = f.listFiles(); if (ks != null) for (File k : ks) deleteTree(k); f.delete(); }
    static void xattrsOf(File f, String rel, StringBuilder out) {
        try { for (String n : Os.listxattr(f.getPath())) { byte[] v = Os.getxattr(f.getPath(), n); out.append(rel).append('\t').append(n).append('\t').append(hex(v)).append('\n'); } }
        catch (Exception e) { }   // a file system without xattrs
    }
    static void copyXattrsTree(File s, File d) {
        try { for (String n : Os.listxattr(s.getPath())) Os.setxattr(d.getPath(), n, Os.getxattr(s.getPath(), n), 0); } catch (Exception e) { Log.w(TAG, "xattr " + s + ": " + e); }
        String[] ns = s.isDirectory() ? s.list() : null; if (ns != null) for (String n : ns) copyXattrsTree(new File(s, n), new File(d, n));
    }
    static Map<String, String[]> readXattrs(File f) {   // path relative to the backup → {name, hex value} rows
        Map<String, String[]> m = new LinkedHashMap<>(); byte[] b = readAll(f); if (b == null) return m; int i = 0;
        for (String line : new String(b, StandardCharsets.UTF_8).split("\n")) { String[] c = line.split("\t"); if (c.length == 3) m.put(c[0] + "\u0000" + (i++), c); }
        return m;
    }
    static void applyXattrs(File root, String top, Map<String, String[]> xa) {   // the user namespace alone: a backup's security.selinux rows are its phone's labels
        for (String[] c : xa.values()) {
            if (!c[1].startsWith("user.") || !c[0].equals(top) && !c[0].startsWith(top + "/")) continue;
            File f = new File(root, c[0].substring(top.length()));
            try { Os.setxattr(f.getPath(), c[1], unhex(c[2]), 0); } catch (Exception e) { Log.w(TAG, "xattr " + f + ": " + e); }
        }
    }
    static String hex(byte[] v) { StringBuilder s = new StringBuilder(); for (byte x : v) s.append(String.format("%02x", x & 0xFF)); return s.toString(); }
    static byte[] unhex(String s) { byte[] b = new byte[s.length() / 2]; for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16); return b; }
    static void writeText(File f, String s) throws IOException { f.getParentFile().mkdirs(); try (FileOutputStream o = new FileOutputStream(f)) { o.write(s.getBytes(StandardCharsets.UTF_8)); } }

    // ---- helpers ----
    void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_LONG).show(); Log.i(TAG, "launcher: " + s); }
    int px(float v) { return Math.round(v * dp); }
    TextView text(String s, float sp, int color, int weight) {   // weight 400, 500 (medium) or 700
        TextView t = new TextView(this); t.setText(s); t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp); t.setTextColor(color); t.setIncludeFontPadding(false);
        t.setTypeface(weight >= 700 ? Typeface.create("sans-serif", Typeface.BOLD) : weight >= 500 ? Typeface.create("sans-serif-medium", Typeface.NORMAL) : Typeface.create("sans-serif", Typeface.NORMAL));
        return t;
    }
    TextView caps(String s) { TextView t = text(s.toUpperCase(Locale.ROOT), 11, TEXT2, 500); t.setLetterSpacing(0.12f); return t; }   // a section's label
    TextView pill(String s, int bg, int fg) {   // a rounded button: filled (bg) or quiet (bg 0)
        TextView t = text(s, 13, fg, 500); t.setGravity(Gravity.CENTER); t.setPadding(px(16), 0, px(16), 0); t.setMinHeight(px(36));
        if (bg != 0) t.setBackground(round(bg, 999, 0)); press(t); return t;
    }
    GradientDrawable round(int color, float radiusDp, int stroke) {
        GradientDrawable d = new GradientDrawable(); d.setColor(color); d.setCornerRadius(px(radiusDp)); if (stroke != 0) d.setStroke(Math.max(1, px(1)), stroke); return d;
    }
    GradientDrawable dot(int color) { GradientDrawable d = new GradientDrawable(); d.setShape(GradientDrawable.OVAL); d.setColor(color); d.setBounds(0, 0, px(8), px(8)); return d; }
    android.graphics.drawable.Drawable glyph(float[] pts, int color, boolean fill) {   // a 16 dp glyph from points on a 16-unit grid: a filled triangle (Play) or a chevron
        android.graphics.Path p = new android.graphics.Path(); p.moveTo(pts[0], pts[1]); for (int i = 2; i < pts.length; i += 2) p.lineTo(pts[i], pts[i + 1]); if (fill) p.close();
        android.graphics.drawable.ShapeDrawable d = new android.graphics.drawable.ShapeDrawable(new android.graphics.drawable.shapes.PathShape(p, 16, 16));
        d.getPaint().setColor(color); d.getPaint().setAntiAlias(true);
        if (!fill) { d.getPaint().setStyle(android.graphics.Paint.Style.STROKE); d.getPaint().setStrokeWidth(1.6f); d.getPaint().setStrokeCap(android.graphics.Paint.Cap.ROUND); d.getPaint().setStrokeJoin(android.graphics.Paint.Join.ROUND); }
        d.setBounds(0, 0, px(16), px(16)); return d;
    }
    View art(Game g, float radiusDp) {   // the game's icon, rounded, with a hairline edge; its name on a tile when it has none
        Bitmap bm = BitmapFactory.decodeFile(new File(g.dir, "icon.jpg").getPath());
        if (bm == null) { TextView t = text(g.name, 13, TEXT2, 500); t.setGravity(Gravity.BOTTOM); t.setPadding(px(14), px(14), px(14), px(14)); t.setBackground(round(LIFT, radiusDp, LINE)); return t; }
        ImageView i = new ImageView(this); i.setImageBitmap(bm); i.setScaleType(ImageView.ScaleType.CENTER_CROP);
        i.setBackground(round(LIFT, radiusDp, 0)); i.setClipToOutline(true); i.setForeground(round(0, radiusDp, LINE));
        return i;
    }
    void press(View v) {   // the tactile answer to a touch: a slight shrink while held
        v.setOnTouchListener(new View.OnTouchListener() { public boolean onTouch(View x, android.view.MotionEvent e) {
            int a = e.getActionMasked();
            if (a == android.view.MotionEvent.ACTION_DOWN) x.animate().scaleX(0.97f).scaleY(0.97f).setDuration(110).start();
            else if (a == android.view.MotionEvent.ACTION_UP || a == android.view.MotionEvent.ACTION_CANCEL) x.animate().scaleX(1).scaleY(1).setDuration(160).start();
            return false; } });
    }
    // DroidPlay's icon beside the wordmark: the light green square and the controller of apk/res/drawable/ic_launcher_fg.xml, on the same 1024-unit grid as DroidPlay's
    // Mac icon (the controller scaled by 1.5161 about the centre, as there)
    static final class Mark extends View {
        final android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        final android.graphics.Path joyL = new android.graphics.Path(), joyR = new android.graphics.Path(), play = new android.graphics.Path();
        Mark(android.content.Context c) {
            super(c);
            joyL.addRoundRect(new android.graphics.RectF(255, 322, 415, 702), new float[] {80, 80, 0, 0, 0, 0, 80, 80}, android.graphics.Path.Direction.CW);
            joyR.addRoundRect(new android.graphics.RectF(609, 322, 769, 702), new float[] {0, 0, 80, 80, 80, 80, 0, 0}, android.graphics.Path.Direction.CW);
            play.moveTo(490, 476); play.lineTo(490, 548); play.lineTo(550, 512); play.close();
        }
        void dots(android.graphics.Canvas c, int color, float[] xyr) { p.setColor(color); for (int i = 0; i < xyr.length; i += 3) c.drawCircle(xyr[i], xyr[i + 1], xyr[i + 2], p); }
        @Override protected void onDraw(android.graphics.Canvas c) {
            float s = Math.min(getWidth(), getHeight()) / 1024f; c.save(); c.scale(s, s);
            p.setStyle(android.graphics.Paint.Style.FILL); p.setShader(new android.graphics.LinearGradient(0, 0, 0, 1024, 0xFF9EE8AD, 0xFF47BA75, android.graphics.Shader.TileMode.CLAMP));
            c.drawRoundRect(0, 0, 1024, 1024, 228, 228, p); p.setShader(null);
            c.translate(512, 512); c.scale(1.5161f, 1.5161f); c.translate(-512, -512);
            p.setColor(0xFF3A3F4B); c.drawRoundRect(413, 334, 611, 690, 26, 26, p);
            p.setColor(0xFF00B8D9); c.drawPath(joyL, p); dots(c, 0xFF0A5F70, new float[] {335, 432, 36, 335, 556, 15, 335, 624, 15, 301, 590, 15, 369, 590, 15});
            p.setColor(0xFFFF4F5E); c.drawPath(joyR, p); dots(c, 0xFF8A1D27, new float[] {689, 398, 15, 689, 466, 15, 655, 432, 15, 723, 432, 15, 689, 590, 36});
            p.setColor(0xFFFFFFFF); p.setStyle(android.graphics.Paint.Style.FILL_AND_STROKE); p.setStrokeWidth(26); p.setStrokeJoin(android.graphics.Paint.Join.ROUND); c.drawPath(play, p);
            c.restore();
        }
    }
    static String pct(float v) { return Math.round(v * 100) + "%"; }
    static float parse(String s, float d) { try { return Float.parseFloat(s); } catch (Exception e) { return d; } }
    static int indexOf(String[] a, String v) { for (int i = 0; i < a.length; i++) if (a[i].equals(v)) return i; return -1; }
    static byte[] titles(byte[] nacp) {   // the title area: newer SDKs compress it (a u16 size + raw deflate, 32 languages x 0x300: nacp.py name)
        if (nacp != null && nacp.length > 2) try {
            java.util.zip.Inflater z = new java.util.zip.Inflater(true); z.setInput(nacp, 2, Math.min((nacp[0] & 0xFF) | (nacp[1] & 0xFF) << 8, nacp.length - 2));
            byte[] t = new byte[0x6000]; int n = 0; while (n < t.length && !z.finished()) { int k = z.inflate(t, n, t.length - n); if (k == 0 && (z.needsInput() || z.needsDictionary())) break; n += k; }
            z.end(); if (n == 0x6000) return t;
        } catch (java.util.zip.DataFormatException e) { }   // not compressed
        return nacp;
    }
    static String title(byte[] nacp, String fallback) {   // the first language entry with a name (0x300 bytes each: name 0x200, publisher 0x100)
        byte[] t = titles(nacp);
        if (t != null) for (int i = 0; i < 16 && (i + 1) * 0x300 <= t.length; i++) {
            int o = i * 0x300, e = o; while (e < o + 0x200 && t[e] != 0) e++;
            if (e > o) return new String(t, o, e - o, StandardCharsets.UTF_8);
        }
        return fallback;
    }
    static String displayVersion(byte[] nacp) {   // DisplayVersion, 16 bytes at 0x3060 (nacp.py version)
        if (nacp == null || nacp.length < 0x3070) return null;
        int e = 0x3060; while (e < 0x3070 && nacp[e] != 0) e++;
        return e > 0x3060 ? new String(nacp, 0x3060, e - 0x3060, StandardCharsets.UTF_8) : null;
    }
    // game.json's size of the RomFS, whose image is in another folder (build_android_from_nsp.py --romfs-dir): 0 when it can't be read
    static long romfsBytes(File d) {
        try { return new org.json.JSONObject(new String(readAll(new File(d, "game.json")), StandardCharsets.UTF_8)).optLong("romfs_bytes", 0); } catch (Exception e) { return 0; }
    }
    static byte[] readAll(File f) {
        try (FileInputStream in = new FileInputStream(f)) { byte[] b = new byte[(int) f.length()]; int n = 0; while (n < b.length) { int k = in.read(b, n, b.length - n); if (k < 0) break; n += k; } return b; }
        catch (IOException e) { return null; }
    }
    static Map<String, String> readKeys(File f) {   // KEY=VALUE per line, # comments (as front_android.c env_file)
        Map<String, String> m = new LinkedHashMap<>(); byte[] b = readAll(f); if (b == null) return m;
        for (String line : new String(b, StandardCharsets.UTF_8).split("\r?\n")) { int eq = line.indexOf('='); if (line.startsWith("#") || eq <= 0) continue; m.put(line.substring(0, eq), line.substring(eq + 1)); }
        return m;
    }
}
