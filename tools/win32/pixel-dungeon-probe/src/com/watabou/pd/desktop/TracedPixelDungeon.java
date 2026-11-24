package com.watabou.pd.desktop;

import com.badlogic.gdx.Gdx;
import com.badlogic.gdx.graphics.Pixmap;
import com.watabou.gdx.GdxTexture;
import com.watabou.noosa.BitmapText;
import com.watabou.noosa.Game;
import com.watabou.noosa.NoosaScript;
import com.watabou.pixeldungeon.PixelDungeon;
import com.watabou.pixeldungeon.input.GameAction;
import com.watabou.pixeldungeon.scenes.PixelScene;
import com.watabou.utils.BitmapCache;
import com.watabou.utils.PDPlatformSupport;
import com.watabou.utils.SystemTime;
import java.nio.ByteBuffer;

final class TracedPixelDungeon extends PixelDungeon {
    private static final String FONT_CHARS =
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ" +
        "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\u007f";

    private boolean traceStep;
    private boolean firstScene = true;
    private boolean firstDraw = true;
    private boolean firstRender = true;
    private int platformSupportQueries;

    TracedPixelDungeon(PDPlatformSupport<GameAction> platform) {
        super(platform);
    }

    @Override
    public PDPlatformSupport<GameAction> getPlatformSupport() {
        PDPlatformSupport<GameAction> platform = super.getPlatformSupport();
        boolean fullscreenEnabled =
            platform != null && platform.isFullscreenEnabled();
        DebugDesktopLauncher.log(
            "game platform query=" + (++platformSupportQueries) +
            " support=" +
            (platform == null ? "<null>" : platform.getClass().getName()) +
            " fullscreen=" + fullscreenEnabled);
        return platform;
    }

    @Override
    public void render() {
        if (Game.width == 0 || Game.height == 0) {
            return;
        }

        boolean trace = firstRender;
        firstRender = false;

        SystemTime.tick();
        long nextNow = SystemTime.now;
        this.step = this.now == 0 ? 0 : nextNow - this.now;
        this.now = nextNow;

        step();

        if (trace) {
            DebugDesktopLauncher.log("game resetCamera begin");
        }
        NoosaScript.get().resetCamera();
        if (trace) {
            DebugDesktopLauncher.log("game resetCamera end");
            DebugDesktopLauncher.log("game glScissor begin");
        }
        Gdx.gl.glScissor(0, 0, Game.width, Game.height);
        if (trace) {
            DebugDesktopLauncher.log("game glScissor end");
            DebugDesktopLauncher.log("game glClear begin");
        }
        Gdx.gl.glClear(0x4000);
        if (trace) {
            DebugDesktopLauncher.log("game glClear end");
        }

        draw();
    }

    @Override
    protected void step() {
        traceStep = firstScene;
        if (traceStep) {
            DebugDesktopLauncher.log("game step begin");
        }
        super.step();
        if (traceStep) {
            DebugDesktopLauncher.log("game step end");
        }
        traceStep = false;
    }

    @Override
    protected void switchScene() {
        DebugDesktopLauncher.log("game switchScene begin");
        if (firstScene) {
            firstScene = false;
            traceFonts();
        }
        DebugDesktopLauncher.log("game switchScene super begin");
        super.switchScene();
        DebugDesktopLauncher.log("game switchScene super end");
    }

    @Override
    protected void update() {
        if (traceStep) {
            DebugDesktopLauncher.log("game update begin");
        }
        super.update();
        if (traceStep) {
            DebugDesktopLauncher.log("game update end");
        }
    }

    @Override
    protected void draw() {
        boolean trace = firstDraw;
        firstDraw = false;
        if (trace) {
            DebugDesktopLauncher.log("game draw begin");
        }
        super.draw();
        if (trace) {
            DebugDesktopLauncher.log("game draw end");
        }
    }

    private static GdxTexture texture(String name) {
        DebugDesktopLauncher.log("font texture begin " + name);
        tracePixmap(name);
        GdxTexture result = BitmapCache.get(name);
        DebugDesktopLauncher.log("font texture end " + name);
        return result;
    }

    private static void tracePixmap(String name) {
        Pixmap pixmap = null;
        try {
            pixmap = new Pixmap(Gdx.files.internal(name));
            ByteBuffer pixels = pixmap.getPixels().duplicate();
            int position = pixels.position();
            int limit = pixels.limit();
            long hash = 0xcbf29ce484222325L;
            StringBuilder head = new StringBuilder();
            for (int i = position; i < limit; i++) {
                int value = pixels.get(i) & 0xff;
                hash ^= value;
                hash *= 0x100000001b3L;
                if (i - position < 16) {
                    if (head.length() != 0) head.append(' ');
                    if (value < 0x10) head.append('0');
                    head.append(Integer.toHexString(value));
                }
            }
            DebugDesktopLauncher.log(
                "font pixmap " + name + " " + pixmap.getWidth() + "x" +
                pixmap.getHeight() + " format=" + pixmap.getFormat() +
                " pos=" + position + " limit=" + limit + " fnv=" +
                Long.toHexString(hash) + " head=" + head);
        } catch (Throwable error) {
            DebugDesktopLauncher.reportThrowable(
                "font pixmap probe failed " + name,
                Thread.currentThread(), error);
        } finally {
            if (pixmap != null) pixmap.dispose();
        }
    }

    private static void traceFonts() {
        if (PixelScene.font1x != null) {
            DebugDesktopLauncher.log("fonts already initialized");
            return;
        }

        DebugDesktopLauncher.log("font colorMarked begin font1x.png");
        PixelScene.font1x = BitmapText.Font.colorMarked(
            texture("font1x.png"), 0, FONT_CHARS);
        PixelScene.font1x.baseLine = 6.0f;
        PixelScene.font1x.tracking = -1.0f;
        DebugDesktopLauncher.log("font colorMarked end font1x.png");

        DebugDesktopLauncher.log("font colorMarked begin font15x.png");
        PixelScene.font15x = BitmapText.Font.colorMarked(
            texture("font15x.png"), 12, 0, FONT_CHARS);
        PixelScene.font15x.baseLine = 9.0f;
        PixelScene.font15x.tracking = -1.0f;
        DebugDesktopLauncher.log("font colorMarked end font15x.png");

        DebugDesktopLauncher.log("font colorMarked begin font2x.png");
        PixelScene.font2x = BitmapText.Font.colorMarked(
            texture("font2x.png"), 14, 0, FONT_CHARS);
        PixelScene.font2x.baseLine = 11.0f;
        PixelScene.font2x.tracking = -1.0f;
        DebugDesktopLauncher.log("font colorMarked end font2x.png");

        DebugDesktopLauncher.log("font colorMarked begin font25x.png");
        PixelScene.font25x = BitmapText.Font.colorMarked(
            texture("font25x.png"), 17, 0, FONT_CHARS);
        PixelScene.font25x.baseLine = 13.0f;
        PixelScene.font25x.tracking = -1.0f;
        DebugDesktopLauncher.log("font colorMarked end font25x.png");

        DebugDesktopLauncher.log("font colorMarked begin font3x.png");
        PixelScene.font3x = BitmapText.Font.colorMarked(
            texture("font3x.png"), 22, 0, FONT_CHARS);
        PixelScene.font3x.baseLine = 17.0f;
        PixelScene.font3x.tracking = -2.0f;
        DebugDesktopLauncher.log("font colorMarked end font3x.png");
    }
}
