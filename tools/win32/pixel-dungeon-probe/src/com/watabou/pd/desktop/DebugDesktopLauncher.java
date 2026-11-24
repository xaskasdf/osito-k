package com.watabou.pd.desktop;

import com.badlogic.gdx.ApplicationListener;
import com.badlogic.gdx.Files;
import com.badlogic.gdx.Preferences;
import com.badlogic.gdx.backends.lwjgl.LwjglApplication;
import com.badlogic.gdx.backends.lwjgl.LwjglApplicationConfiguration;
import com.badlogic.gdx.backends.lwjgl.LwjglPreferences;
import com.badlogic.gdx.backends.lwjgl.TracedLwjglApplication;
import com.badlogic.gdx.utils.SharedLibraryLoader;
import com.codedisaster.steamworks.SteamAPI;
import com.watabou.pixeldungeon.PixelDungeon;
import com.watabou.utils.PDPlatformSupport;
import java.net.URL;
import java.security.CodeSource;
import org.lwjgl.opengl.GLContext;

public final class DebugDesktopLauncher {
    private DebugDesktopLauncher() {}

    public static void log(String message) {
        System.err.println("[OSITO-PD] " + message);
        System.err.flush();
    }

    private static void safePrint(String value) {
        try {
            System.err.print(value);
        } catch (Throwable ignored) {
        }
    }

    private static void safePrintln(String value) {
        try {
            System.err.println(value);
            System.err.flush();
        } catch (Throwable ignored) {
        }
    }

    public static void reportThrowable(String context, Thread thread,
                                       Throwable error) {
        safePrint("[OSITO-PD] ");
        safePrint(context);
        if (thread != null) {
            safePrint(" thread=");
            try {
                safePrint(thread.getName());
            } catch (Throwable ignored) {
                safePrint("<unavailable>");
            }
        }
        safePrintln("");

        if (error == null) {
            safePrintln("[OSITO-PD] throwable=<null>");
            return;
        }

        safePrint("[OSITO-PD] type=");
        try {
            safePrintln(error.getClass().getName());
        } catch (Throwable ignored) {
            safePrintln("<unavailable>");
        }
        safePrint("[OSITO-PD] message=");
        try {
            safePrintln(String.valueOf(error.getMessage()));
        } catch (Throwable ignored) {
            safePrintln("<unavailable>");
        }

        try {
            StackTraceElement[] frames = error.getStackTrace();
            int count = frames == null ? 0 : frames.length;
            safePrint("[OSITO-PD] frames=");
            safePrintln(Integer.toString(count));
            for (int i = 0; i < count && i < 64; i++) {
                safePrint("[OSITO-PD] at ");
                try {
                    safePrintln(String.valueOf(frames[i]));
                } catch (Throwable ignored) {
                    safePrintln("<unavailable>");
                }
            }
        } catch (Throwable reporterError) {
            safePrint("[OSITO-PD] stack-report-failed=");
            try {
                safePrintln(reporterError.getClass().getName());
            } catch (Throwable ignored) {
                safePrintln("<unavailable>");
            }
        }

        try {
            Throwable cause = error.getCause();
            for (int depth = 1;
                 cause != null && cause != error && depth <= 8;
                 depth++) {
                safePrint("[OSITO-PD] cause[");
                safePrint(Integer.toString(depth));
                safePrint("] type=");
                safePrintln(cause.getClass().getName());
                safePrint("[OSITO-PD] cause[");
                safePrint(Integer.toString(depth));
                safePrint("] message=");
                safePrintln(String.valueOf(cause.getMessage()));

                StackTraceElement[] causeFrames = cause.getStackTrace();
                int causeCount = causeFrames == null ? 0 : causeFrames.length;
                for (int i = 0; i < causeCount && i < 32; i++) {
                    safePrint("[OSITO-PD] caused at ");
                    safePrintln(String.valueOf(causeFrames[i]));
                }

                Throwable next = cause.getCause();
                if (next == cause) {
                    break;
                }
                cause = next;
            }
        } catch (Throwable reporterError) {
            safePrint("[OSITO-PD] cause-report-failed=");
            try {
                safePrintln(reporterError.getClass().getName());
            } catch (Throwable ignored) {
                safePrintln("<unavailable>");
            }
        }
    }

    private static ApplicationListener traced(final ApplicationListener app) {
        return new ApplicationListener() {
            private boolean rendered;
            private long frameCount;

            public void create() {
                log("lifecycle create begin");
                app.create();
                log("lifecycle create end");
            }

            public void resize(int width, int height) {
                log("lifecycle resize " + width + "x" + height);
                app.resize(width, height);
            }

            public void render() {
                if (!rendered) {
                    rendered = true;
                    log("lifecycle first render");
                }
                long frame = ++frameCount;
                boolean traceFrame = frame <= 3 || frame % 300 == 0;
                if (traceFrame) {
                    log("lifecycle render begin " + frame);
                }
                app.render();
                if (traceFrame) {
                    log("lifecycle render end " + frame);
                }
            }

            public void pause() {
                log("lifecycle pause");
                app.pause();
            }

            public void resume() {
                log("lifecycle resume");
                app.resume();
            }

            public void dispose() {
                log("lifecycle dispose");
                app.dispose();
            }
        };
    }

    private static String classSource(Class<?> type) {
        try {
            CodeSource codeSource = type.getProtectionDomain().getCodeSource();
            URL location = codeSource == null ? null : codeSource.getLocation();
            return location == null ? "<unknown>" : location.toString();
        } catch (Throwable ignored) {
            return "<unavailable>";
        }
    }

    private static Class<?> probeClass(String name, boolean initialize)
            throws ClassNotFoundException {
        log("class probe begin name=" + name + " initialize=" + initialize);
        Class<?> type = Class.forName(
            name, initialize, DebugDesktopLauncher.class.getClassLoader());
        log("class probe end name=" + name + " source=" + classSource(type));
        return type;
    }

    public static void probeLwjglBootstrap() {
        try {
            probeClass("org.lwjgl.opengl.WindowsContextImplementation", false);
            probeClass("org.lwjgl.opengl.WindowsContextImplementation", true);
            probeClass("org.lwjgl.opengl.ContextGL", true);
            log("GLContext.loadOpenGLLibrary begin");
            GLContext.loadOpenGLLibrary();
            log("GLContext.loadOpenGLLibrary end");
        } catch (Throwable error) {
            reportThrowable(
                "LWJGL bootstrap probe failed", Thread.currentThread(), error);
            throw new RuntimeException(error);
        }
    }

    public static void main(String[] args) {
        Thread.setDefaultUncaughtExceptionHandler(
            new Thread.UncaughtExceptionHandler() {
                public void uncaughtException(Thread thread, Throwable error) {
                    reportThrowable("uncaught", thread, error);
                }
            });

        if (args != null && args.length > 0 &&
                "--osito-steamworks-probe".equals(args[0])) {
            log("running isolated Steamworks probe");
            SteamworksProbe.main(args);
            return;
        }

        boolean steamInitialized = false;
        try {
            log("SteamAPI.init begin");
            steamInitialized = SteamAPI.init();
            log("SteamAPI.init result=" + steamInitialized);
            if (steamInitialized) {
                log("SteamAPI pointers utils=0x" +
                    Long.toHexString(SteamAPI.getSteamUtilsPointer()) +
                    " stats=0x" +
                    Long.toHexString(SteamAPI.getSteamUserStatsPointer()));
            }
        } catch (Throwable error) {
            reportThrowable("SteamAPI initialization failed",
                Thread.currentThread(), error);
        }

        if (steamInitialized) {
            Runtime.getRuntime().addShutdownHook(new Thread("steam-shutdown") {
                @Override
                public void run() {
                    try {
                        SteamAPI.shutdown();
                        log("SteamAPI.shutdown complete");
                    } catch (Throwable error) {
                        reportThrowable("SteamAPI shutdown failed",
                            Thread.currentThread(), error);
                    }
                }
            });
        }

        Package desktopPackage = DesktopLauncher.class.getPackage();
        String version = desktopPackage.getSpecificationVersion();
        if (version == null) {
            version = "???";
        } else {
            version += " - " + desktopPackage.getImplementationVersion();
        }

        LwjglApplicationConfiguration config =
            new LwjglApplicationConfiguration();
        if (SharedLibraryLoader.isMac) {
            config.preferencesDirectory =
                "Library/Application Support/Pixel Dungeon/";
        } else if (SharedLibraryLoader.isLinux) {
            config.preferencesDirectory = ".watabou/pixel-dungeon/";
        } else if (SharedLibraryLoader.isWindows) {
            config.preferencesDirectory = "Saved Games/";
        }

        Preferences preferences = new LwjglPreferences(
            "pd-prefs", config.preferencesDirectory);
        config.fullscreen = preferences.getBoolean("windowFullscreen", false);
        if (!config.fullscreen) {
            config.width = preferences.getInteger("winWidth", 1152);
            config.height = preferences.getInteger("winHeight", 648);
        }
        config.addIcon("ic_launcher_128.png", Files.FileType.Internal);
        config.addIcon("ic_launcher_32.png", Files.FileType.Internal);
        config.addIcon("ic_launcher_16.png", Files.FileType.Internal);
        config.title = "Pixel Dungeon (Osito-K probe)";

        log("launching with Steamworks initialized=" + steamInitialized);
        try {
            PDPlatformSupport platform;
            if (steamInitialized) {
                platform = new DesktopSupport(
                    version, config.preferencesDirectory,
                    new DesktopInputProcessor());
            } else {
                platform = new PDPlatformSupport(
                    version, config.preferencesDirectory,
                    new DesktopInputProcessor()) {
                    private boolean fullscreenCapabilityLogged;

                    @Override
                    public boolean isFullscreenEnabled() {
                        if (!fullscreenCapabilityLogged) {
                            fullscreenCapabilityLogged = true;
                            DebugDesktopLauncher.log(
                                "platform fullscreen capability=true class=" +
                                getClass().getName());
                        }
                        return true;
                    }
                };
            }
            log("platform support=" + platform.getClass().getName() +
                " fullscreen=" + platform.isFullscreenEnabled());
            ApplicationListener game = new TracedPixelDungeon(platform);
            TracedLwjglApplication application =
                new TracedLwjglApplication(traced(game), config);
            log("LwjglApplication constructor returned");
            application.waitForMainLoop();
            log("LWJGL main loop joined");
        } catch (Throwable error) {
            reportThrowable("launcher failed", Thread.currentThread(), error);
            throw error;
        }
    }
}
