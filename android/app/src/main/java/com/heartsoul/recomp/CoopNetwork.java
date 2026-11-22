package com.heartsoul.recomp;

import android.content.Context;

/** Same embedded, controller-less ZeroTier peer network used by PokeWilds Mobile. */
final class CoopNetwork {
    static final int PORT = 54555;

    static final class Status {
        final String state;
        final String address;
        final String message;
        Status(String value) {
            String[] parts = value == null ? new String[0] : value.split("\\t", 3);
            state = parts.length > 0 ? parts[0] : "error";
            address = parts.length > 1 ? parts[1] : "";
            message = parts.length > 2 ? parts[2] : "";
        }
        boolean ready() { return "ready".equals(state) && !address.isEmpty(); }
        boolean failed() { return "error".equals(state); }
    }

    private CoopNetwork() {}

    static boolean available() {
        return HeartSoulZeroTier.available();
    }

    /** Starts the ad-hoc network once. No app, account, or network setup is needed. */
    static String start(Context context) {
        if (!available()) return "Embedded ZeroTier is missing from this build.";
        return HeartSoulZeroTier.start(context.getFilesDir().getAbsolutePath() + "/zerotier", PORT);
    }

    static Status status() {
        return new Status(HeartSoulZeroTier.status());
    }

    /** Forward the host's ZeroTier listener to the local game TCP listener. */
    static boolean hostOpen() {
        return HeartSoulZeroTier.hostOpen(PORT, PORT);
    }

    /** Return a local TCP port that forwards to the remote host over ZeroTier. */
    static int joinOpen(String remoteAddress) {
        if (remoteAddress == null || !remoteAddress.matches("[0-9A-Fa-f:]+")) return 0;
        return HeartSoulZeroTier.connectOpen(remoteAddress, PORT);
    }

    static void close() {
        HeartSoulZeroTier.hostClose();
        HeartSoulZeroTier.connectClose();
    }
}
