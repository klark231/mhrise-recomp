package dev.rise.audit.panel;
import android.app.*;
import android.content.*;
import android.graphics.PixelFormat;
import android.hardware.display.*;
import android.media.*;
import android.os.*;
import android.view.*;
import java.nio.ByteBuffer;
import java.lang.reflect.*;

public class Probe extends Instrumentation {
    static { System.loadLibrary("panel_probe"); }
    static native int start(Surface surf, String dir, String mode, boolean textured, boolean contrast, int passes);
    static native void frame();
    static native void light(boolean enable);
    static native void stop();
    Bundle args;
    @Override
    public void onCreate(Bundle b) {
        args = b;
        start();
    }
    int width, height, step, sw, sh;
    volatile Surface base;
    WindowManager wm;
    Presentation scene;
    View pad;
    android.widget.FrameLayout panelRoot;
    volatile boolean hardwareCanvas;
    VirtualDisplay display;
    ImageReader sink;
    boolean forward;
    int sent, callbacks;
    volatile int padDraws;
    long inputHash;
    boolean fixed, textured;
    LuaHudProbe luaHud;
    Method publish;
    Throwable uiError;
    void ui(Runnable r) throws Exception {
        runOnMainSync(() -> {
            try {
                r.run();
            } catch (Throwable e) {
                uiError = e;
            }
        });
        if (uiError != null)
            throw new Exception(uiError);
    }
    void attach() {
        Context c = getTargetContext();
        sink = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 4);
        DisplayManager dm = (DisplayManager)c.getSystemService(Context.DISPLAY_SERVICE);
        display = dm.createVirtualDisplay("Rise private panel audit", width, height, 240, sink.getSurface(),
                                          DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION |
                                              DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY);
        scene =
            new Presentation(c, display.getDisplay(), android.R.style.Theme_Material_NoActionBar_Fullscreen);
        Window w = scene.getWindow();
        w.setFormat(PixelFormat.RGB_565);
        w.getDecorView().setSystemUiVisibility(5894);
        w.takeSurface(new SurfaceHolder.Callback2() {
            public void surfaceCreated(SurfaceHolder h) {}
            public void surfaceChanged(SurfaceHolder h, int f, int x, int y) { base = h.getSurface(); }
            public void surfaceDestroyed(SurfaceHolder h) { base = null; }
            public void surfaceRedrawNeeded(SurfaceHolder h) {}
        });
        scene.show();
    }
    void attachPad() {
        try {
            Context c = scene.getContext();
            ClassLoader loader = getTargetContext().getClassLoader();
            String pkg = "io.github.iamwilmar.mhriseport.";
            Class<?> cfg = loader.loadClass(pkg + "Controls"), controls = loader.loadClass(pkg + "TouchPad"),
                     host = loader.loadClass(pkg + "TouchPad$Host");
            Constructor<?> make = cfg.getDeclaredConstructor(Context.class);
            make.setAccessible(true);
            Object settings = make.newInstance(c);
            Class<?> game = Class.forName(pkg + "GameActivity", true, loader);
            publish = game.getDeclaredMethod("nativeTouch", long.class, int.class, float.class, float.class,
                                             float.class, float.class);
            publish.setAccessible(true);
            Object proxy = Proxy.newProxyInstance(loader, new Class[] {host}, (o, m, a) -> {
                if (m.getName().equals("send")) {
                    callbacks++;
                    inputHash = 31 * inputHash + java.util.Arrays.hashCode(a);
                    if (forward) {
                        publish.invoke(null, a);
                        sent++;
                    }
                }
                if (m.getName().equals("modTouch") && luaHud != null && (Integer)a[0] == MotionEvent.ACTION_DOWN)
                    return luaHud.windowAt((Float)a[2], (Float)a[3]);
                if (m.getReturnType() == boolean.class)
                    return false;
                return null;
            });
            Constructor<?> ctor = fixed ? controls.getDeclaredConstructor(Context.class, cfg, host, boolean.class)
                                        : controls.getDeclaredConstructor(Context.class, cfg, host);
            ctor.setAccessible(true);
            pad = (View)(fixed ? ctor.newInstance(c, settings, proxy, false) : ctor.newInstance(c, settings, proxy));
            int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN |
                        WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS;
            if (!"cpu".equals(args.getString("panel")))
                flags |= WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED;
            WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                -1, -1, WindowManager.LayoutParams.TYPE_APPLICATION_PANEL, flags, PixelFormat.TRANSLUCENT);
            lp.token = scene.getWindow().getDecorView().getWindowToken();
            lp.setTitle("controles audit");
            panelRoot = new android.widget.FrameLayout(c) {
                @Override
                protected void dispatchDraw(android.graphics.Canvas canvas) {
                    hardwareCanvas = canvas.isHardwareAccelerated();
                    super.dispatchDraw(canvas);
                }
            };
            if ("cpu".equals(args.getString("panel")))
                panelRoot.setLayerType(View.LAYER_TYPE_SOFTWARE, null);
            panelRoot.addView(pad);
            wm = scene.getWindow().getWindowManager();
            wm.addView(panelRoot, lp);
            panelRoot.getViewTreeObserver().addOnDrawListener(() -> padDraws++);
            if (args.containsKey("lua")) {
                luaHud = new LuaHudProbe(c,loader);
                luaHud.attach(wm,scene.getWindow().getDecorView().getWindowToken());
            }
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }
    void event(int action, float x, float y) {
        long t = SystemClock.uptimeMillis();
        MotionEvent e = MotionEvent.obtain(t - 10, t, action, x * pad.getWidth(), y * pad.getHeight(), 0);
        pad.dispatchTouchEvent(e);
        e.recycle();
    }
    int[] sample(Image out) {
        Image.Plane p = out.getPlanes()[0];
        ByteBuffer b = p.getBuffer();
        int[] rgb = new int[sw * sh];
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                int k = y * step * p.getRowStride() + x * step * p.getPixelStride();
                rgb[y * sw + x] = ((b.get(k) & 255) << 16) | ((b.get(k + 1) & 255) << 8) | (b.get(k + 2) & 255);
            }
        return rgb;
    }
    int[] image() throws Exception {
        long until = SystemClock.elapsedRealtime() + 1700;
        Image out = null;
        do {
            out = sink.acquireLatestImage();
            if (out == null)
                SystemClock.sleep(4);
        } while (out == null && SystemClock.elapsedRealtime() < until);
        if (out == null)
            throw new IllegalStateException("virtual display did not deliver a composite");
        try {
            return sample(out);
        } finally {
            out.close();
        }
    }
    void drain() {
        Image i = sink.acquireLatestImage();
        if (i != null)
            i.close();
    }
    void finishUiFrame() throws Exception {
        var fence = new java.util.concurrent.CountDownLatch(1);
        ui(() -> android.view.Choreographer.getInstance().postFrameCallback(t ->
                android.view.Choreographer.getInstance().postFrameCallback(next -> fence.countDown())));
        if (!fence.await(2,java.util.concurrent.TimeUnit.SECONDS)) throw new IllegalStateException("UI frame stalled");
    }
    // Hash visible RGBA bytes only; the compositor may pad the rows of each buffer.
    private String composedDigest(boolean saveSamples) throws Exception {
        finishUiFrame();
        SystemClock.sleep(50);
        drain();
        frame();
        long until = SystemClock.elapsedRealtime() + 1800;
        Image raw;
        while ((raw=sink.acquireLatestImage()) == null && SystemClock.elapsedRealtime() < until) SystemClock.sleep(2);
        if (raw == null) throw new IllegalStateException("missing settled composite");
        try (Image captured=raw) {
            Image.Plane plane = captured.getPlanes()[0];
            if (plane.getPixelStride() != 4) throw new IllegalStateException("unexpected pixel stride");
            ByteBuffer bytes = plane.getBuffer();
            if (saveSamples) {
                try (var out = new java.io.DataOutputStream(new java.io.BufferedOutputStream(
                        new java.io.FileOutputStream(new java.io.File(getTargetContext().getFilesDir(),"idle-samples.rgb"))))) {
                    for(int rgb:sample(captured)) out.writeInt(rgb);
                }
            }
            var digest = java.security.MessageDigest.getInstance("SHA-256");
            for (int y=0; y<height; ++y) {
                ByteBuffer row = bytes.duplicate();
                int start=y*plane.getRowStride();
                row.position(start).limit(start+width*4);
                digest.update(row);
            }
            return android.util.Base64.encodeToString(digest.digest(), android.util.Base64.NO_WRAP);
        }
    }
    int[] loadOracle(String name) throws Exception {
        int[] pixels = new int[sw*sh];
        try (var in = new java.io.DataInputStream(new java.io.BufferedInputStream(
                new java.io.FileInputStream(new java.io.File(getTargetContext().getFilesDir(), name + ".rgb"))))) {
            for (int i=0;i<pixels.length;i++) pixels[i]=in.readInt();
            if (in.read()!=-1) throw new IllegalStateException("oracle size " + name);
        }
        return pixels;
    }
    boolean loadStep(Bundle results) throws Exception {
        int[] linear=loadOracle("linear"), fsr=loadOracle("fsr");
        if (java.util.Arrays.equals(linear,fsr)) throw new IllegalStateException("indistinguishable references");
        int draws=padDraws, input=callbacks, unknown=0;
        int[] linearCount=new int[3], fsrCount=new int[3];
        StringBuilder detail=new StringBuilder();
        for (int phase=0;phase<3;phase++) {
            long startNs=System.nanoTime();
            light(phase==1);
            for (int i=0;i<(phase==1?145:32);i++) {
                frame();
                int[] now=image();
                if (java.util.Arrays.equals(now,linear)) linearCount[phase]++;
                else if (java.util.Arrays.equals(now,fsr)) fsrCount[phase]++;
                else unknown++;
            }
            detail.append("\nload_phase=").append(phase).append(" linear=").append(linearCount[phase])
                  .append(" fsr=").append(fsrCount[phase]).append(" start_ns=").append(startNs)
                  .append(" end_ns=").append(System.nanoTime());
        }
        results.putString("audit",detail+"\nuncategorized="+unknown+" panel_draws="+(padDraws-draws)+" callbacks="+(callbacks-input));
        return unknown==0 && padDraws==draws && callbacks==input && linearCount[0]>0 &&
                fsrCount[1]>0 && linearCount[2]>0;
    }
    @Override
    public void onStart() {
        fixed = "static".equals(args.getString("feedback"));
        boolean contrast = "contrast".equals(args.getString("scene"));
        textured = contrast || "draw".equals(args.getString("scene"));
        width = Integer.parseInt(args.getString("width", "760"));
        height = Integer.parseInt(args.getString("height", "476"));
        step = Math.max(2, width / 380);
        sw = width / step;
        sh = height / step;
        Bundle results = new Bundle();
        int status = Activity.RESULT_CANCELED;
        boolean opened = false;
        try {
            ui(this::attach);
            for (int i = 0; i < 150 && base == null; i++)
                SystemClock.sleep(15);
            if (base == null)
                throw new IllegalStateException("no base Surface");
            int passes=Integer.parseInt(args.getString("passes","1"));
            if (passes < 1) throw new IllegalArgumentException("passes must be positive");
            int rc = start(base, getTargetContext().getFilesDir().getAbsolutePath(), args.getString("mode", "gmem"),
                           textured,contrast,passes);
            if (rc != 0)
                throw new IllegalStateException("native presenter " + rc);
            opened = true;
            frame();
            SystemClock.sleep(100);
            image();
            ui(this::attachPad);
            if (luaHud != null && !luaHud.ready.await(10, java.util.concurrent.TimeUnit.SECONDS))
                throw new IllegalStateException("Lua GLES layer did not submit its first frame");
            SystemClock.sleep(200);
            frame();
            SystemClock.sleep(60);
            int[] ref = image();
            if (passes > 1) {
                // Let the presenter's GPU timing hysteresis settle at this fixed workload before taking the reference.
                for (int i=0;i<72;i++) {frame();image();}
                frame(); ref=image();
            }
            if (args.containsKey("load_step")) {
                if (loadStep(results)) status=Activity.RESULT_OK;
                return;
            }
            StringBuilder log = new StringBuilder();
            log.append("panel ")
                .append(pad.getWidth())
                .append("x")
                .append(pad.getHeight())
                .append(" hardware_canvas=")
                .append(hardwareCanvas)
                .append(" RGB stripes ");
            if (hardwareCanvas == "cpu".equals(args.getString("panel")))
                throw new IllegalStateException("unexpected control Canvas " + hardwareCanvas);
            for (int y : new int[] {sh / 5, sh / 2, 4 * sh / 5})
                log.append(Integer.toHexString(ref[y * sw + sw / 2])).append(" ");
            int wrong = 0, seen = 0, referenceErrors = 0;
            // Three strips from transparent through opaque guest alpha; keep clear of controls.
            if (!textured) for (int y : new int[] {sh / 5, sh / 2, 4 * sh / 5}) {
                int rgb = ref[y * sw + sw / 2];
                if (Math.abs((rgb >> 16) - 64) > 2 || Math.abs(((rgb >> 8) & 255) - 160) > 2 ||
                    Math.abs((rgb & 255) - 224) > 2)
                    referenceErrors++;
            }
            if (textured) {
                // The two stripes are separated by a depth test. Sample their interiors in the displayed image.
                int min = 255, max = 0;
                for (int x=sw*42/100; x<sw*55/100; x++) {
                    int green=(ref[sh/2*sw+x]>>8)&255;
                    min=Math.min(min,green); max=Math.max(max,green);
                }
                if (contrast ? min > 8 || max < 80 : Math.abs(min-64)>4 || Math.abs(max-112)>4) referenceErrors++;
            }
            java.util.concurrent.atomic.AtomicBoolean running =
                new java.util.concurrent.atomic.AtomicBoolean(true);
            Thread producer = new Thread(() -> {
                while (running.get()) {
                    frame();
                    SystemClock.sleep(4);
                }
            }, "NVN present");
            producer.start();
            try {
                // Test identical-buffer redraw before any contact can still be in the display queue.
                for (int phase : new int[]{0, 3, 1, 2, 4}) {
                    int mode = phase;
                    forward = (mode == 2 || mode == 4);
                    int phaseBad = 0, visual = 0, cb = callbacks, send = sent;
                    int maxRgbDelta = 0, becameBlack = 0;
                    int draws = padDraws;
                    int luaFrames = luaHud != null ? luaHud.frames : 0;
                    inputHash = 0;
                    long startNs = System.nanoTime();
                    // Capture successive composite buffers throughout input and panel updates.
                    drain();
                    for (int i = 0; i < 48; i++) {
                        final int step = i % 6;
                        final int heldAction = i == 0 ? MotionEvent.ACTION_DOWN : i == 47 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_MOVE;
                        ui(() -> {
                            if (luaHud != null && "redraw".equals(args.getString("lua"))) luaHud.requestRender();
                            if (mode == 0) {
                                try {
                                    publish.invoke(null, step == 0 ? 1L : 0L, 1, step * .12f, 0f, 0f, 0f);
                                    sent++;
                                } catch (Exception e) {
                                    throw new RuntimeException(e);
                                }
                            } else if (mode == 3) {
                                pad.invalidate(); // identical glyphs, new Android panel frame, no input
                            } else if (mode == 4) {
                                event(heldAction,.935f,.46f);
                            } else if (step < 2)
                                event(step == 0 ? 0 : 1, .935f, .46f);
                            else
                                event(step == 2 ? 0 : step == 5 ? 1 : 2, .12f + (step - 2) * .013f, .46f);
                        });
                        long limit = SystemClock.elapsedRealtime() + 1800;
                        Image delivered;
                        while ((delivered = sink.acquireNextImage()) == null &&
                               SystemClock.elapsedRealtime() < limit)
                            SystemClock.sleep(1);
                        if (delivered == null)
                            throw new IllegalStateException("no frame in phase " + mode);
                        int[] now;
                        try {
                            now = sample(delivered);
                        } finally {
                            delivered.close();
                        }
                        boolean wide = args.containsKey("wide");
                        for (int y = wide ? 0 : sh/6; y < (wide ? sh : sh*5/6); y++)
                            for (int x = wide ? 0 : sw*42/100; x < (wide ? sw : sw*55/100); x++) {
                                if (wide && Math.abs(y - .46f*sh) < .22f*sh &&
                                    (Math.abs(x - .12f*sw) < .22f*sh || Math.abs(x - .935f*sw) < .10f*sh)) continue;
                                seen++;
                                int rgb = now[y*sw+x], original = ref[y*sw+x];
                                if (rgb != original) {
                                    phaseBad++;
                                    int delta = 0;
                                    for (int shift=0; shift<=16; shift+=8)
                                        delta = Math.max(delta,Math.abs(((rgb>>shift)&255)-((original>>shift)&255)));
                                    maxRgbDelta = Math.max(maxRgbDelta,delta);
                                    if ((rgb&0xf8f8f8)==0 && (original&0xe0e0e0)!=0) becameBlack++;
                                }
                            }
                        boolean changed = false;
                        for (int y = sh * 42 / 100; y < sh * 50 / 100; y++)
                            for (int x = 0; x < sw; x++) {
                                if ((x > sw * .90 && x < sw * .97) || (x > sw * .08 && x < sw * .16))
                                    changed |= now[y * sw + x] != ref[y * sw + x];
                            }
                        if (changed)
                            visual++;
                    }
                    finishUiFrame();
                    wrong += phaseBad;
                    if ((mode == 1 || mode == 2 || mode == 4) && (callbacks == cb || (phaseBad == 0 && (fixed ? visual != 0 : visual == 0))))
                        throw new IllegalStateException("positive control missing " + log);
                    if ((mode == 0 || fixed && mode != 3) && padDraws != draws)
                        throw new IllegalStateException("unexpected panel redraw in phase " + mode + ": " + (padDraws-draws));
                    if (mode == 3 && (padDraws == draws || callbacks != cb || (phaseBad == 0 && visual != 0)))
                        throw new IllegalStateException("redraw-only control failed: " + (padDraws-draws) + "/" + visual);
                    log.append("\nphase=")
                        .append(mode)
                        .append(" center_diff=")
                        .append(phaseBad)
                        .append(" max_rgb_delta=").append(maxRgbDelta).append(" became_black=").append(becameBlack)
                        .append(" panel_frames_changed=")
                        .append(visual)
                        .append(" callbacks=")
                        .append(callbacks - cb)
                        .append(" forwarded=")
                        .append(sent - send)
                        .append(" panel_draws=").append(padDraws-draws)
                        .append(" lua_frames=").append(luaHud != null ? luaHud.frames-luaFrames : 0)
                        .append(" start_ns=").append(startNs).append(" end_ns=").append(System.nanoTime())
                        .append(" input_hash=").append(Long.toHexString(inputHash));
                }
            } finally {
                running.set(false);
                producer.join(3000);
                if (producer.isAlive())
                    throw new IllegalStateException("presenter stalled");
            }
            if (args.containsKey("wide")) {
                log.append("\nidle_rgba=").append(composedDigest(true));
                ui(() -> event(MotionEvent.ACTION_DOWN,.935f,.46f));
                log.append("\npressed_rgba=").append(composedDigest(false));
                ui(() -> {event(MotionEvent.ACTION_UP,.935f,.46f);event(MotionEvent.ACTION_DOWN,.12f,.46f);
                          event(MotionEvent.ACTION_MOVE,.17f,.46f);});
                log.append("\nstick_rgba=").append(composedDigest(false));
                ui(() -> event(MotionEvent.ACTION_UP,.17f,.46f));
                log.append("\nreleased_rgba=").append(composedDigest(false));
            }
            results.putString("audit", log + "\nchecked=" + seen + " center_diff=" + wrong +
                                           " scene_reference_errors=" + referenceErrors);
            if (wrong == 0 && referenceErrors == 0)
                status = Activity.RESULT_OK;
        } catch (Throwable e) {
            results.putString("error", android.util.Log.getStackTraceString(e));
        } finally {
            if (opened)
                stop();
            try {
                ui(() -> {
                    if (luaHud != null && luaHud.isAttachedToWindow()) luaHud.detach(wm);
                    if (panelRoot != null && panelRoot.isAttachedToWindow())
                        wm.removeViewImmediate(panelRoot);
                    if (scene != null)
                        scene.dismiss();
                    if (display != null)
                        display.release();
                    if (sink != null)
                        sink.close();
                });
            } catch (Exception e) {
                results.putString("cleanup", e.toString());
            }
            finish(status, results);
        }
    }
}
