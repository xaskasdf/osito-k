package com.badlogic.gdx.backends.lwjgl;

import com.badlogic.gdx.ApplicationListener;
import com.watabou.pd.desktop.DebugDesktopLauncher;

public final class TracedLwjglApplication extends LwjglApplication {
    public TracedLwjglApplication(ApplicationListener listener,
                                  LwjglApplicationConfiguration config) {
        super(listener, config, new TracedLwjglGraphics(config));
    }

    @Override
    void mainLoop() {
        System.err.println("[OSITO-PD] main loop enter");
        System.err.flush();
        try {
            super.mainLoop();
            System.err.println("[OSITO-PD] main loop returned");
            System.err.flush();
        } catch (Throwable error) {
            DebugDesktopLauncher.reportThrowable(
                "main loop failed", Thread.currentThread(), error);
            if (error instanceof RuntimeException) {
                throw (RuntimeException)error;
            }
            if (error instanceof Error) {
                throw (Error)error;
            }
            throw new RuntimeException(error);
        }
    }

    public void waitForMainLoop() {
        try {
            mainLoopThread.join();
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            DebugDesktopLauncher.reportThrowable(
                "main loop join interrupted", Thread.currentThread(), error);
        }
    }
}
