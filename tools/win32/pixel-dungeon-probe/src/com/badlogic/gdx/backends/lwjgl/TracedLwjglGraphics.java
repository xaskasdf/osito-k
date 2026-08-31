package com.badlogic.gdx.backends.lwjgl;

import com.badlogic.gdx.Files;
import com.badlogic.gdx.Gdx;
import com.badlogic.gdx.Graphics;
import com.badlogic.gdx.graphics.Pixmap;
import com.badlogic.gdx.utils.GdxRuntimeException;
import com.watabou.pd.desktop.DebugDesktopLauncher;
import java.nio.ByteBuffer;
import org.lwjgl.LWJGLException;
import org.lwjgl.input.Keyboard;
import org.lwjgl.input.Mouse;
import org.lwjgl.opengl.ContextAttribs;
import org.lwjgl.opengl.Display;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.PixelFormat;

final class TracedLwjglGraphics extends LwjglGraphics {
    private static final boolean BYPASS_WINDOW_EXTRAS = false;

    TracedLwjglGraphics(LwjglApplicationConfiguration config) {
        super(config);
    }

    @Override
    void setupDisplay() throws LWJGLException {
        System.err.println("[OSITO-PD] graphics setup begin");
        System.err.flush();
        try {
            DebugDesktopLauncher.probeLwjglBootstrap();
            setupDisplayTraced();
            System.err.println("[OSITO-PD] graphics setup returned");
            System.err.flush();
        } catch (Throwable error) {
            DebugDesktopLauncher.reportThrowable(
                "graphics setup failed", Thread.currentThread(), error);
            if (error instanceof LWJGLException) {
                throw (LWJGLException)error;
            }
            if (error instanceof RuntimeException) {
                throw (RuntimeException)error;
            }
            if (error instanceof Error) {
                throw (Error)error;
            }
            throw new RuntimeException(error);
        }
    }

    private void setupDisplayTraced() throws LWJGLException {
        if (BYPASS_WINDOW_EXTRAS) {
            System.err.println(
                "[OSITO-PD] bypassing LWJGL input and window icons");
            System.err.flush();
            System.setProperty("org.lwjgl.opengl.Display.noinput", "true");
        }
        if (config.useHDPI) {
            System.setProperty("org.lwjgl.opengl.Display.enableHighDPI", "true");
        }
        if (canvas != null) {
            Display.setParent(canvas);
        } else {
            boolean displayCreated =
                setDisplayMode(config.width, config.height, config.fullscreen);
            if (!displayCreated && config.setDisplayModeCallback != null) {
                config = config.setDisplayModeCallback.onFailure(config);
                if (config != null) {
                    displayCreated = setDisplayMode(
                        config.width, config.height, config.fullscreen);
                }
            }
            if (!displayCreated) {
                throw new GdxRuntimeException(
                    "Couldn't set display mode " + config.width + "x" +
                    config.height + ", fullscreen: " + config.fullscreen);
            }

            if (!BYPASS_WINDOW_EXTRAS && config.iconPaths.size > 0) {
                ByteBuffer[] icons = new ByteBuffer[config.iconPaths.size];
                for (int i = 0, n = config.iconPaths.size; i < n; i++) {
                    Pixmap pixmap = new Pixmap(Gdx.files.getFileHandle(
                        config.iconPaths.get(i), config.iconFileTypes.get(i)));
                    if (pixmap.getFormat() != Pixmap.Format.RGBA8888) {
                        Pixmap rgba = new Pixmap(
                            pixmap.getWidth(), pixmap.getHeight(),
                            Pixmap.Format.RGBA8888);
                        rgba.drawPixmap(pixmap, 0, 0);
                        pixmap = rgba;
                    }
                    icons[i] = ByteBuffer.allocateDirect(
                        pixmap.getPixels().limit());
                    icons[i].put(pixmap.getPixels()).flip();
                    pixmap.dispose();
                }
                Display.setIcon(icons);
            }
        }

        Display.setTitle(config.title);
        Display.setResizable(config.resizable);
        Display.setInitialBackground(
            config.initialBackgroundColor.r,
            config.initialBackgroundColor.g,
            config.initialBackgroundColor.b);
        Display.setLocation(config.x, config.y);
        if (BYPASS_WINDOW_EXTRAS) {
            Display.setIcon(new ByteBuffer[0]);
        }
        createDisplayPixelFormatTraced();
        initiateGLInstances();
    }

    private void traceFailure(String attempt, Throwable error) {
        DebugDesktopLauncher.reportThrowable(
            "Display.create " + attempt + " failed",
            Thread.currentThread(), error);
    }

    private void createDisplayPixelFormatTraced() {
        traceInputState("before Display.create", false);
        try {
            System.err.println("[OSITO-PD] Display.create primary begin");
            System.err.flush();
            if (config.useGL30) {
                ContextAttribs context = new ContextAttribs(3, 2)
                    .withForwardCompatible(false).withProfileCore(true);
                Display.create(new PixelFormat(
                    config.r + config.g + config.b, config.a, config.depth,
                    config.stencil, config.samples), context);
            } else {
                Display.create(new PixelFormat(
                    config.r + config.g + config.b, config.a, config.depth,
                    config.stencil, config.samples));
            }
            System.err.println("[OSITO-PD] Display.create primary end");
            System.err.flush();
            traceInputState("after Display.create primary", true);
            bufferFormat = new Graphics.BufferFormat(
                config.r, config.g, config.b, config.a, config.depth,
                config.stencil, config.samples, false);
            return;
        } catch (Exception first) {
            traceFailure("primary", first);
        }

        Display.destroy();
        sleepAfterFailure();
        try {
            System.err.println("[OSITO-PD] Display.create fallback-16-8 begin");
            System.err.flush();
            Display.create(new PixelFormat(0, 16, 8));
            System.err.println("[OSITO-PD] Display.create fallback-16-8 end");
            System.err.flush();
            traceInputState("after Display.create fallback-16-8", true);
            int bpp = getDesktopDisplayMode().bitsPerPixel;
            if (bpp == 16) {
                bufferFormat = new Graphics.BufferFormat(
                    5, 6, 5, 0, 16, 8, 0, false);
            } else if (bpp == 24) {
                bufferFormat = new Graphics.BufferFormat(
                    8, 8, 8, 0, 16, 8, 0, false);
            } else if (bpp == 32) {
                bufferFormat = new Graphics.BufferFormat(
                    8, 8, 8, 8, 16, 8, 0, false);
            }
            return;
        } catch (Exception second) {
            traceFailure("fallback-16-8", second);
        }

        Display.destroy();
        sleepAfterFailure();
        try {
            System.err.println("[OSITO-PD] Display.create fallback-default begin");
            System.err.flush();
            Display.create(new PixelFormat());
            System.err.println("[OSITO-PD] Display.create fallback-default end");
            System.err.flush();
            traceInputState("after Display.create fallback-default", true);
        } catch (Exception third) {
            traceFailure("fallback-default", third);
            if (!softwareMode && config.allowSoftwareMode) {
                softwareMode = true;
                System.setProperty(
                    "org.lwjgl.opengl.Display.allowSoftwareOpenGL", "true");
                createDisplayPixelFormatTraced();
                return;
            }
            String glInfo = traceGlInfo();
            throw new GdxRuntimeException(
                "OpenGL is not supported by the video driver" +
                (glInfo.isEmpty() ? "." : ":" + glInfo), third);
        }

        int bpp = getDesktopDisplayMode().bitsPerPixel;
        if (bpp == 16) {
            bufferFormat = new Graphics.BufferFormat(
                5, 6, 5, 0, 8, 0, 0, false);
        } else if (bpp == 24) {
            bufferFormat = new Graphics.BufferFormat(
                8, 8, 8, 0, 8, 0, 0, false);
        } else if (bpp == 32) {
            bufferFormat = new Graphics.BufferFormat(
                8, 8, 8, 8, 8, 0, 0, false);
        }
    }

    private static void traceInputState(String phase, boolean createIfMissing) {
        System.err.println(
            "[OSITO-PD] input " + phase +
            " noinput=" + System.getProperty("org.lwjgl.opengl.Display.noinput") +
            " nomouse=" + System.getProperty("org.lwjgl.opengl.Display.nomouse") +
            " nokeyboard=" + System.getProperty("org.lwjgl.opengl.Display.nokeyboard") +
            " display=" + Display.isCreated() +
            " mouse=" + Mouse.isCreated() +
            " keyboard=" + Keyboard.isCreated());
        System.err.flush();

        if (!createIfMissing || !Display.isCreated() || Mouse.isCreated()) {
            return;
        }

        try {
            System.err.println("[OSITO-PD] explicit Mouse.create begin");
            System.err.flush();
            Mouse.create();
            System.err.println(
                "[OSITO-PD] explicit Mouse.create end created=" +
                Mouse.isCreated());
            System.err.flush();
        } catch (Throwable error) {
            DebugDesktopLauncher.reportThrowable(
                "explicit Mouse.create failed", Thread.currentThread(), error);
        }
    }

    private static void sleepAfterFailure() {
        try {
            Thread.sleep(200L);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }

    private static String traceGlInfo() {
        try {
            return GL11.glGetString(GL11.GL_VENDOR) + "\n" +
                GL11.glGetString(GL11.GL_RENDERER) + "\n" +
                GL11.glGetString(GL11.GL_VERSION);
        } catch (Throwable ignored) {
            return "";
        }
    }
}
