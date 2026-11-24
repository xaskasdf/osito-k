package com.watabou.pd.desktop;

import com.codedisaster.steamworks.SteamAPI;
import com.codedisaster.steamworks.SteamUtils;

public final class SteamworksProbe {
    private SteamworksProbe() {}

    private static void log(String message) {
        System.err.println("[OSITO-STEAMWORKS] " + message);
        System.err.flush();
    }

    private static String value(String name) {
        String result = System.getenv(name);
        return result == null ? "<unset>" : result;
    }

    public static void main(String[] args) {
        log("cwd=" + System.getProperty("user.dir"));
        log("tmp=" + System.getProperty("java.io.tmpdir"));
        log("SteamAppId=" + value("SteamAppId"));
        log("SteamGameId=" + value("SteamGameId"));
        log("SteamClientLaunch=" + value("SteamClientLaunch"));

        boolean initialized = false;
        try {
            log("SteamAPI.init begin");
            initialized = SteamAPI.init();
            log("SteamAPI.init result=" + initialized);
            if (!initialized) {
                return;
            }

            log("SteamAPI.isSteamRunning=" + SteamAPI.isSteamRunning());
            long utilsPointer = SteamAPI.getSteamUtilsPointer();
            long statsPointer = SteamAPI.getSteamUserStatsPointer();
            log("SteamUtils pointer=0x" + Long.toHexString(utilsPointer));
            log("SteamUserStats pointer=0x" + Long.toHexString(statsPointer));
            if (utilsPointer != 0) {
                SteamUtils utils = new SteamUtils(utilsPointer);
                log("AppID=" + utils.getAppID());
                log("overlay=" + utils.isOverlayEnabled());
            }

            SteamAPI.runCallbacks();
            log("SteamAPI.runCallbacks complete");
        } catch (Throwable error) {
            DebugDesktopLauncher.reportThrowable(
                "Steamworks probe failed", Thread.currentThread(), error);
        } finally {
            if (initialized) {
                try {
                    SteamAPI.shutdown();
                    log("SteamAPI.shutdown complete");
                } catch (Throwable error) {
                    DebugDesktopLauncher.reportThrowable(
                        "Steamworks shutdown failed",
                        Thread.currentThread(), error);
                }
            }
        }
    }
}
