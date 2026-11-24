package dev.rise.audit.panel;

import android.content.Context;
import android.graphics.PixelFormat;
import android.opengl.GLSurfaceView;
import android.view.WindowManager;
import java.lang.reflect.Method;
import java.util.concurrent.CountDownLatch;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

// Use the merged APK's GL backend in its own translucent window. No game or Lua scripts are started.
final class LuaHudProbe extends GLSurfaceView implements GLSurfaceView.Renderer {
    final CountDownLatch ready = new CountDownLatch(1);
    private final Method create, size, frame, close, hit;
    private int width, height;
    volatile int frames;

    LuaHudProbe(Context c, ClassLoader loader) throws Exception {
        super(c);
        Class<?> mod = loader.loadClass("io.github.iamwilmar.mhriseport.ModUi");
        create = method(mod, "nativeCreate", float.class);
        size = method(mod, "nativeSize", int.class, int.class);
        frame = method(mod, "nativeFrame", int.class, int.class);
        close = method(mod, "nativeClose");
        hit = method(mod, "nativeWindowAt", float.class, float.class);
        setEGLContextClientVersion(3);
        setEGLConfigChooser(8, 8, 8, 8, 0, 0);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
        setZOrderOnTop(true);
        setRenderer(this);
        setRenderMode(RENDERMODE_WHEN_DIRTY);
    }
    private static Method method(Class<?> type, String name, Class<?>... args) throws Exception {
        Method m = type.getDeclaredMethod(name, args);
        m.setAccessible(true);
        return m;
    }
    private static Object call(Method m, Object... args) {
        try {
            return m.invoke(null, args);
        } catch (ReflectiveOperationException ex) {
            throw new IllegalStateException(m.getName(), ex);
        }
    }
    boolean windowAt(float x, float y) { return (Boolean)call(hit, x, y); }
    void attach(WindowManager wm, android.os.IBinder token) {
        int flags =
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE |
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS;
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
            -1, -1, WindowManager.LayoutParams.TYPE_APPLICATION_PANEL, flags, PixelFormat.TRANSLUCENT);
        lp.token = token;
        lp.setTitle("Lua HUD audit");
        wm.addView(this, lp);
    }
    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        call(create, getResources().getDisplayMetrics().density);
    }
    @Override
    public void onSurfaceChanged(GL10 gl, int w, int h) {
        width = w;
        height = h;
        call(size, w, h);
    }
    @Override
    public void onDrawFrame(GL10 gl) {
        call(frame, width, height);
        frames++;
        ready.countDown();
    }
    void detach(WindowManager wm) {
        queueEvent(() -> call(close));
        onPause();
        wm.removeViewImmediate(this);
    }
}
