package com.heartsoul.recomp;

import android.content.Context;
import android.util.Log;

import com.zerotier.sockets.ZeroTierEventListener;
import com.zerotier.sockets.ZeroTierNative;
import com.zerotier.sockets.ZeroTierSocket;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.Arrays;

/**
 * Phone-to-phone link. The game keeps using ordinary TCP. This class only
 * carries those bytes across a controller-less ZeroTier network, and tells a
 * small directory where this phone can be reached.
 *
 *   host:   ZeroTier :27770  ->  127.0.0.1:27770
 *   joiner: 127.0.0.1:ephemeral  ->  host ZeroTier IPv6 :27770
 */
final class HeartSoulLink {
    private static final String TAG = "HeartSoulLink";
    static final int GAME_PORT = 27770;
    private static final int EAGAIN = 11;
    private static final int ONLINE_WAIT_MS = 60000;
    private static final int NETWORK_WAIT_MS = 90000;
    private static final int CONNECT_WAIT_MS = 15000;
    private static final int PACE = 4096;

    private static final Object GATE = new Object();
    private static final Object ZT = new Object();
    private static final InetAddress LOOPBACK = loopback();

    private static volatile String phase = "stopped";
    private static volatile String address = "";
    private static volatile String detail = "";
    private static volatile long networkId;
    private static volatile boolean ready;
    private static boolean nodeStarted;

    private static volatile int hostEpoch;
    private static volatile int joinEpoch;
    private static ServerSocket joinDoor;
    private static volatile boolean announce = true;
    private static Thread announcer;

    private HeartSoulLink() {}

    static final class Outcome {
        final boolean ok;
        final String message;
        final int localPort;

        Outcome(boolean ok, String message, int localPort) {
            this.ok = ok;
            this.message = message;
            this.localPort = localPort;
        }
    }

    private static InetAddress loopback() {
        try {
            return InetAddress.getByAddress(new byte[] {127, 0, 0, 1});
        } catch (Exception impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    /** Brings the node up and opens the host bridge. Announces when a directory URL is set. */
    static Outcome prepareHost(Context context) {
        String problem = boot(context);
        if (problem != null) {
            return new Outcome(false, problem, 0);
        }
        if (!waitUntilReady()) {
            return new Outcome(false, detail.isEmpty() ? "The phone link did not come up." : detail, 0);
        }
        openHostBridge();
        startAnnouncer(context.getApplicationContext());
        return new Outcome(true, "", 0);
    }

    /** Finds the one listed host, or the newest if several are listed, and opens a loopback port. */
    static Outcome prepareJoin(Context context) {
        String directory = BuildConfig.LOBBY_URL == null ? "" : BuildConfig.LOBBY_URL.trim();
        if (directory.isEmpty()) {
            return new Outcome(false, "The game list is not configured yet.", 0);
        }
        String problem = boot(context);
        if (problem != null) {
            return new Outcome(false, problem, 0);
        }
        if (!waitUntilReady()) {
            return new Outcome(false, detail.isEmpty() ? "The phone link did not come up." : detail, 0);
        }
        String host = pollDirectory(directory);
        if (host == null) {
            return new Outcome(false, "No game is listed right now.", 0);
        }
        int port = openJoinBridge(host, GAME_PORT);
        if (port <= 0) {
            return new Outcome(false, "Could not open the local bridge.", 0);
        }
        return new Outcome(true, "", port);
    }

    static void shutdown() {
        announce = false;
        hostEpoch++;
        joinEpoch++;
        synchronized (GATE) {
            if (joinDoor != null) {
                try {
                    joinDoor.close();
                } catch (Exception ignored) {
                }
                joinDoor = null;
            }
        }
    }

    private static String boot(Context context) {
        synchronized (GATE) {
            if (nodeStarted) {
                return null;
            }
            nodeStarted = true;
        }
        final long id;
        try {
            id = ZeroTierNative.zts_net_compute_adhoc_id((short) GAME_PORT, (short) GAME_PORT);
        } catch (Throwable error) {
            Log.w(TAG, "libzt is missing", error);
            nodeStarted = false;
            return "This build does not include the phone link.";
        }
        networkId = id;
        phase = "starting";
        detail = "";
        File storage = new File(context.getFilesDir(), "zt");
        Thread thread = new Thread(() -> bringUp(storage, id), "hs-zt-start");
        thread.setDaemon(true);
        thread.start();
        return null;
    }

    private static void bringUp(File storage, long id) {
        try {
            if (!storage.isDirectory() && !storage.mkdirs()) {
                fail("Phone link storage could not be created.");
                return;
            }
            ZeroTierNative.zts_init_from_storage(storage.getAbsolutePath());
            ZeroTierNative.zts_init_set_event_handler(new ZeroTierEventListener() {
                @Override
                public void onZeroTierEvent(long eventId, int code) {
                    noteEvent(code);
                }
            });
            int error = ZeroTierNative.zts_node_start();
            if (error != ZeroTierNative.ZTS_ERR_OK) {
                fail("The phone link could not start.");
                return;
            }
            if (!await(ONLINE_WAIT_MS, false)) {
                fail("Could not reach the phone link. Check the internet connection.");
                return;
            }
            phase = "joining";
            ZeroTierNative.zts_net_join(id);
            if (!await(NETWORK_WAIT_MS, true)) {
                if (!"error".equals(phase)) {
                    fail("The phone link network did not respond.");
                }
                return;
            }
            String assigned = ZeroTierNative.zts_addr_get_str(id, ZeroTierNative.ZTS_AF_INET6);
            if (assigned == null || assigned.isEmpty()) {
                fail("The phone link did not assign an address.");
                return;
            }
            address = assigned;
            detail = "";
            phase = "ready";
            ready = true;
        } catch (Throwable error) {
            Log.e(TAG, "phone link failed to start", error);
            fail("The phone link failed to start.");
        }
    }

    private static boolean await(int timeoutMs, boolean network) throws InterruptedException {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            if ("error".equals(phase)) {
                return false;
            }
            if (network) {
                if (ZeroTierNative.zts_net_transport_is_ready(networkId) == 1
                        && ZeroTierNative.zts_addr_is_assigned(networkId, ZeroTierNative.ZTS_AF_INET6) == 1) {
                    return true;
                }
            } else if (ZeroTierNative.zts_node_is_online() == 1) {
                return true;
            }
            Thread.sleep(100);
        }
        return false;
    }

    private static boolean waitUntilReady() {
        long deadline = System.currentTimeMillis() + NETWORK_WAIT_MS + ONLINE_WAIT_MS;
        while (System.currentTimeMillis() < deadline) {
            if (ready) {
                return true;
            }
            if ("error".equals(phase)) {
                return false;
            }
            try {
                Thread.sleep(100);
            } catch (InterruptedException stopped) {
                return false;
            }
        }
        return ready;
    }

    private static void noteEvent(int code) {
        if (code == ZeroTierNative.ZTS_EVENT_NETWORK_NOT_FOUND) {
            fail("The phone link network was not found.");
        } else if (code == ZeroTierNative.ZTS_EVENT_NETWORK_ACCESS_DENIED) {
            fail("The phone link network refused this device.");
        } else if (code == ZeroTierNative.ZTS_EVENT_NETWORK_CLIENT_TOO_OLD) {
            fail("This game version is too old for the phone link.");
        } else if (code == ZeroTierNative.ZTS_EVENT_NODE_FATAL_ERROR) {
            fail("Phone link identity error. Reinstalling resets it.");
        }
    }

    private static void fail(String reason) {
        ready = false;
        detail = reason;
        phase = "error";
        Log.w(TAG, reason);
    }

    private interface Live {
        boolean current();
    }

    private static void openHostBridge() {
        final int epoch = ++hostEpoch;
        Thread thread = new Thread(() -> hostLoop(new Live() {
            @Override
            public boolean current() {
                return hostEpoch == epoch;
            }
        }), "hs-zt-host");
        thread.setDaemon(true);
        thread.start();
    }

    private static void hostLoop(Live live) {
        ZeroTierSocket server;
        try {
            server = new ZeroTierSocket(ZeroTierNative.ZTS_AF_INET6, ZeroTierNative.ZTS_SOCK_STREAM, 0);
        } catch (Exception error) {
            Log.e(TAG, "host socket failed", error);
            return;
        }
        int listener = server.getNativeFileDescriptor();
        try {
            ZeroTierNative.zts_set_reuse_addr(listener, 1);
            if (ZeroTierNative.zts_bind(listener, "::", GAME_PORT) < 0) {
                Log.e(TAG, "host bind failed");
                return;
            }
            server.listen(4);
            ZeroTierNative.zts_set_blocking(listener, 0);
            while (live.current()) {
                int accepted;
                try {
                    synchronized (ZT) {
                        accepted = server.accept().getNativeFileDescriptor();
                    }
                } catch (Exception waiting) {
                    Thread.sleep(25);
                    continue;
                }
                if (accepted < 0 || accepted == listener) {
                    Thread.sleep(25);
                    continue;
                }
                Socket local = connectLocalWithRetry();
                if (local == null) {
                    ztClose(accepted);
                    continue;
                }
                pump("host", local, accepted, live);
            }
        } catch (Exception stopped) {
            Log.w(TAG, "host bridge stopped", stopped);
        } finally {
            ztClose(listener);
        }
    }

    private static Socket connectLocalWithRetry() {
        long deadline = System.currentTimeMillis() + 20000;
        while (System.currentTimeMillis() < deadline) {
            Socket local = new Socket();
            try {
                local.connect(new InetSocketAddress(LOOPBACK, GAME_PORT), 1000);
                return local;
            } catch (Exception refused) {
                closeQuietly(local);
                try {
                    Thread.sleep(200);
                } catch (InterruptedException stopped) {
                    return null;
                }
            }
        }
        Log.w(TAG, "game server was not listening");
        return null;
    }

    private static int openJoinBridge(final String remoteAddress, final int remotePort) {
        joinEpoch++;
        final int epoch = joinEpoch;
        final ServerSocket listener;
        try {
            listener = new ServerSocket(0, 2, LOOPBACK);
            listener.setSoTimeout(500);
        } catch (Exception error) {
            Log.e(TAG, "loopback listener failed", error);
            return 0;
        }
        synchronized (GATE) {
            joinDoor = listener;
        }
        Thread thread = new Thread(() -> joinLoop(listener, remoteAddress, remotePort, new Live() {
            @Override
            public boolean current() {
                return joinEpoch == epoch;
            }
        }), "hs-zt-join");
        thread.setDaemon(true);
        thread.start();
        return listener.getLocalPort();
    }

    private static void joinLoop(ServerSocket listener, String remoteAddress, int remotePort, Live live) {
        try {
            while (live.current()) {
                Socket local;
                try {
                    local = listener.accept();
                } catch (SocketTimeoutException waiting) {
                    continue;
                }
                if (!ready) {
                    closeQuietly(local);
                    continue;
                }
                int remote = ZeroTierNative.zts_bsd_socket(ZeroTierNative.ZTS_AF_INET6, ZeroTierNative.ZTS_SOCK_STREAM, 0);
                if (remote < 0) {
                    closeQuietly(local);
                    continue;
                }
                int error = ZeroTierNative.zts_connect(remote, remoteAddress, remotePort, CONNECT_WAIT_MS);
                if (error < 0) {
                    Log.w(TAG, "phone connect failed " + error);
                    ztClose(remote);
                    closeQuietly(local);
                    continue;
                }
                pump("join", local, remote, live);
            }
        } catch (Exception closed) {
            // Listener closed on shutdown.
        } finally {
            try {
                listener.close();
            } catch (Exception ignored) {
            }
        }
    }

    private static void pump(final String role, final Socket local, final int remote, final Live live) {
        Thread thread = new Thread(() -> {
            try {
                local.setTcpNoDelay(true);
                local.setSoTimeout(1);
                ZeroTierNative.zts_set_no_delay(remote, 1);
                ZeroTierNative.zts_set_blocking(remote, 0);
                InputStream in = local.getInputStream();
                OutputStream out = local.getOutputStream();
                byte[] up = new byte[PACE];
                byte[] down = new byte[32768];
                while (live.current()) {
                    boolean moved = false;
                    if (in.available() > 0) {
                        int count = in.read(up);
                        if (count < 0) {
                            break;
                        }
                        if (count > 0) {
                            if (!writeAll(remote, up, count)) {
                                break;
                            }
                            moved = true;
                            if (in.available() > 0) {
                                Thread.sleep(1);
                            }
                        }
                    }
                    int received;
                    synchronized (ZT) {
                        received = ZeroTierNative.zts_bsd_read_length(remote, down, down.length);
                    }
                    if (received > 0) {
                        out.write(down, 0, received);
                        out.flush();
                        moved = true;
                    } else if (received == 0 || (received < 0 && received != -EAGAIN)) {
                        break;
                    }
                    if (!moved) {
                        try {
                            int count = in.read(up);
                            if (count < 0) {
                                break;
                            }
                            if (count > 0 && !writeAll(remote, up, count)) {
                                break;
                            }
                        } catch (SocketTimeoutException idle) {
                            // Nothing this millisecond.
                        }
                    }
                }
            } catch (Exception closed) {
                Log.i(TAG, role + " bridge closed");
            } finally {
                closeQuietly(local);
                ztClose(remote);
            }
        }, "hs-zt-pump");
        thread.setDaemon(true);
        thread.start();
    }

    private static boolean writeAll(int remote, byte[] buffer, int count) throws InterruptedException {
        byte[] pending = count == buffer.length ? buffer : Arrays.copyOf(buffer, count);
        long deadline = System.currentTimeMillis() + 20000;
        while (pending.length > 0) {
            int written;
            synchronized (ZT) {
                written = ZeroTierNative.zts_bsd_write(remote, pending);
            }
            if (written == pending.length) {
                return true;
            }
            if (written > 0) {
                pending = Arrays.copyOfRange(pending, written, pending.length);
            } else if (written == 0 || written == -EAGAIN) {
                if (System.currentTimeMillis() > deadline) {
                    return false;
                }
                Thread.sleep(1);
            } else {
                return false;
            }
        }
        return true;
    }

    private static void ztClose(int fd) {
        synchronized (ZT) {
            ZeroTierNative.zts_bsd_close(fd);
        }
    }

    private static void closeQuietly(Socket socket) {
        try {
            socket.close();
        } catch (Exception ignored) {
        }
    }

    private static void startAnnouncer(Context context) {
        String directory = BuildConfig.LOBBY_URL == null ? "" : BuildConfig.LOBBY_URL.trim();
        if (directory.isEmpty()) {
            return;
        }
        announce = true;
        if (announcer != null && announcer.isAlive()) {
            return;
        }
        announcer = new Thread(() -> {
            while (announce) {
                postAddress(context, directory);
                try {
                    Thread.sleep(30000);
                } catch (InterruptedException stopped) {
                    return;
                }
            }
        }, "hs-lobby");
        announcer.setDaemon(true);
        announcer.start();
    }

    private static void postAddress(Context context, String directory) {
        if (address.isEmpty()) {
            return;
        }
        String key = deviceKey(context);
        if (key.isEmpty()) {
            return;
        }
        String body = "{\"key\":\"" + key + "\",\"address\":\"" + jsonEscape(address) + "\"}";
        exchange(directory, "POST", body);
    }

    /** Returns the ZeroTier address of the newest listed host, or null. */
    private static String pollDirectory(String directory) {
        long deadline = System.currentTimeMillis() + 40000;
        String newest = null;
        long newestSeen = -1;
        while (System.currentTimeMillis() < deadline) {
            String text = exchange(directory, "GET", null);
            newest = null;
            newestSeen = -1;
            int count = 0;
            if (text != null) {
                int from = 0;
                while (true) {
                    int mark = text.indexOf("\"address\"", from);
                    if (mark < 0) {
                        break;
                    }
                    int colon = text.indexOf(':', mark);
                    int quote = text.indexOf('"', colon + 1);
                    int end = text.indexOf('"', quote + 1);
                    if (colon < 0 || quote < 0 || end < 0) {
                        break;
                    }
                    String found = text.substring(quote + 1, end);
                    long seen = seenNear(text, end);
                    count++;
                    if (seen >= newestSeen) {
                        newestSeen = seen;
                        newest = found;
                    }
                    from = end + 1;
                }
                if (count >= 1 && newest != null) {
                    return newest;
                }
            }
            try {
                Thread.sleep(2000);
            } catch (InterruptedException stopped) {
                return null;
            }
        }
        return newest;
    }

    private static long seenNear(String text, int from) {
        int mark = text.indexOf("\"seen\"", from);
        if (mark < 0 || mark > from + 80) {
            return 0;
        }
        int colon = text.indexOf(':', mark);
        if (colon < 0) {
            return 0;
        }
        int i = colon + 1;
        while (i < text.length() && (text.charAt(i) == ' ')) {
            i++;
        }
        long value = 0;
        boolean any = false;
        while (i < text.length() && text.charAt(i) >= '0' && text.charAt(i) <= '9') {
            any = true;
            value = value * 10 + (text.charAt(i) - '0');
            i++;
        }
        return any ? value : 0;
    }

    private static String exchange(String directory, String method, String body) {
        HttpURLConnection connection = null;
        try {
            String url = directory.endsWith("/") ? directory + "v1/hosts" : directory + "/v1/hosts";
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(10000);
            connection.setReadTimeout(10000);
            connection.setUseCaches(false);
            connection.setRequestMethod(method);
            String lobbyKey = BuildConfig.LOBBY_KEY == null ? "" : BuildConfig.LOBBY_KEY;
            if (!lobbyKey.isEmpty()) {
                connection.setRequestProperty("X-HeartSoul-Key", lobbyKey);
            }
            if (body != null) {
                byte[] data = body.getBytes(StandardCharsets.UTF_8);
                connection.setRequestProperty("Content-Type", "application/json");
                connection.setDoOutput(true);
                connection.setFixedLengthStreamingMode(data.length);
                try (OutputStream out = connection.getOutputStream()) {
                    out.write(data);
                }
            }
            int code = connection.getResponseCode();
            InputStream stream = code >= 400 ? connection.getErrorStream() : connection.getInputStream();
            if (stream == null || code >= 400) {
                return null;
            }
            byte[] buf = new byte[4096];
            StringBuilder text = new StringBuilder();
            int count;
            while ((count = stream.read(buf)) > 0 && text.length() < 65536) {
                text.append(new String(buf, 0, count, StandardCharsets.UTF_8));
            }
            stream.close();
            return text.toString();
        } catch (Exception error) {
            Log.w(TAG, "game list request failed");
            return null;
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    private static String deviceKey(Context context) {
        File file = new File(context.getFilesDir(), "coop-lobby.id");
        try {
            if (file.isFile() && file.length() >= 32) {
                byte[] raw = new byte[(int) Math.min(file.length(), 80)];
                try (FileInputStream in = new FileInputStream(file)) {
                    int n = in.read(raw);
                    if (n >= 32) {
                        return new String(raw, 0, n, StandardCharsets.US_ASCII).trim();
                    }
                }
            }
            byte[] random = new byte[32];
            new SecureRandom().nextBytes(random);
            StringBuilder hex = new StringBuilder(64);
            for (byte b : random) {
                hex.append(String.format("%02x", b));
            }
            try (FileOutputStream out = new FileOutputStream(file)) {
                out.write(hex.toString().getBytes(StandardCharsets.US_ASCII));
            }
            return hex.toString();
        } catch (Exception error) {
            return "";
        }
    }

    private static String jsonEscape(String value) {
        return value.replace("\\", "\\\\").replace("\"", "");
    }
}
