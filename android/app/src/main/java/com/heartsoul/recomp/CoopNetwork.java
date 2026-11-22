package com.heartsoul.recomp;

import android.content.Context;
import android.net.wifi.WifiManager;

import org.w3c.dom.Document;
import org.w3c.dom.NodeList;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;
import javax.xml.parsers.DocumentBuilderFactory;

/** Direct phone-to-phone TCP hosting helpers. No lobby, relay, or game server. */
final class CoopNetwork {
    static final int PORT = 54555;
    private static final String ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    private static final long INTERNET_TAG = 0x100000000L;
    private static String upnpServiceType = "urn:schemas-upnp-org:service:WANIPConnection:1";

    static final class HostInfo {
        final String localAddress;
        final String publicAddress;
        final String joinCode;
        final boolean mapped;
        HostInfo(String local, String external, boolean portMapped) {
            localAddress = local;
            publicAddress = external;
            mapped = portMapped;
            joinCode = encode(external.isEmpty() ? local : external, !external.isEmpty());
        }
    }

    private CoopNetwork() {}

    static HostInfo prepareHost(Context context, int port) {
        String local = localAddress();
        boolean mapped = false;
        String external = "";
        try {
            mapped = addUpnpMapping(context, local, port);
            if (mapped) external = upnpExternalAddress(context);
        } catch (Exception ignored) {
            // A router without UPnP can still host after a manual TCP port forward.
        }
        if (external.isEmpty()) external = publicAddress();
        return new HostInfo(local, external, mapped);
    }

    static String localAddress() {
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            while (interfaces.hasMoreElements()) {
                NetworkInterface network = interfaces.nextElement();
                Enumeration<InetAddress> addresses = network.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    InetAddress address = addresses.nextElement();
                    if (address instanceof Inet4Address && !address.isLoopbackAddress()
                            && !address.isLinkLocalAddress() && address.isSiteLocalAddress())
                        return address.getHostAddress();
                }
            }
        } catch (Exception ignored) {}
        return "";
    }

    static String publicAddress() {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL("https://api.ipify.org").openConnection();
            connection.setConnectTimeout(4000);
            connection.setReadTimeout(4000);
            InputStream in = connection.getInputStream();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[128];
            int count;
            while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
            in.close();
            String value = out.toString("UTF-8").trim();
            return value.matches("[0-9]{1,3}(\\.[0-9]{1,3}){3}") ? value : "";
        } catch (Exception ignored) {
            return "";
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    private static boolean addUpnpMapping(Context context, String local, int port) throws Exception {
        if (local.isEmpty()) return false;
        String control = upnpControlUrl(context);
        if (control.isEmpty()) return false;
        String body = "<?xml version=\"1.0\"?>"
                + "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                + "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                + "<u:AddPortMapping xmlns:u=\"" + upnpServiceType + "\">"
                + "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + port + "</NewExternalPort>"
                + "<NewProtocol>TCP</NewProtocol><NewInternalPort>" + port + "</NewInternalPort>"
                + "<NewInternalClient>" + local + "</NewInternalClient><NewEnabled>1</NewEnabled>"
                + "<NewPortMappingDescription>HeartSoul Co-op</NewPortMappingDescription>"
                + "<NewLeaseDuration>3600</NewLeaseDuration></u:AddPortMapping>"
                + "</s:Body></s:Envelope>";
        return soap(control, upnpServiceType + "#AddPortMapping", body) != null;
    }

    private static String upnpExternalAddress(Context context) throws Exception {
        String control = upnpControlUrl(context);
        if (control.isEmpty()) return "";
        String body = "<?xml version=\"1.0\"?>"
                + "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                + "<u:GetExternalIPAddress xmlns:u=\"" + upnpServiceType + "\"/>"
                + "</s:Body></s:Envelope>";
        byte[] response = soap(control,
                upnpServiceType + "#GetExternalIPAddress", body);
        if (response == null) return "";
        Document doc = DocumentBuilderFactory.newInstance().newDocumentBuilder()
                .parse(new java.io.ByteArrayInputStream(response));
        NodeList nodes = doc.getElementsByTagName("NewExternalIPAddress");
        return nodes.getLength() == 0 ? "" : nodes.item(0).getTextContent().trim();
    }

    private static String upnpControlUrl(Context context) throws Exception {
        WifiManager wifi = (WifiManager) context.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        WifiManager.MulticastLock lock = wifi == null ? null : wifi.createMulticastLock("HeartSoulUPnP");
        if (lock != null) lock.acquire();
        try {
            String request = "M-SEARCH * HTTP/1.1\r\n"
                    + "HOST: 239.255.255.250:1900\r\n"
                    + "MAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                    + "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
            DatagramSocket socket = new DatagramSocket();
            socket.setSoTimeout(1800);
            byte[] bytes = request.getBytes(StandardCharsets.US_ASCII);
            socket.send(new DatagramPacket(bytes, bytes.length,
                    InetAddress.getByName("239.255.255.250"), 1900));
            String location = "";
            long until = System.currentTimeMillis() + 1800;
            while (System.currentTimeMillis() < until) {
                byte[] data = new byte[4096];
                DatagramPacket packet = new DatagramPacket(data, data.length);
                try {
                    socket.receive(packet);
                    String response = new String(packet.getData(), 0, packet.getLength(), StandardCharsets.US_ASCII);
                    for (String line : response.split("\\r?\\n")) {
                        if (line.toLowerCase().startsWith("location:")) {
                            location = line.substring(line.indexOf(':') + 1).trim();
                            break;
                        }
                    }
                    if (!location.isEmpty()) break;
                } catch (java.net.SocketTimeoutException timeout) {
                    break;
                }
            }
            socket.close();
            if (location.isEmpty()) return "";
            HttpURLConnection connection = (HttpURLConnection) new URL(location).openConnection();
            connection.setConnectTimeout(3000);
            connection.setReadTimeout(3000);
            InputStream in = connection.getInputStream();
            Document doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(in);
            in.close();
            connection.disconnect();
            NodeList services = doc.getElementsByTagName("service");
            for (int i = 0; i < services.getLength(); i++) {
                org.w3c.dom.Element service = (org.w3c.dom.Element) services.item(i);
                String type = childText(service, "serviceType");
                if (type.contains("WANIPConnection") || type.contains("WANPPPConnection")) {
                    String path = childText(service, "controlURL");
                    if (!path.isEmpty()) {
                        upnpServiceType = type;
                        return new URL(new URL(location), path).toString();
                    }
                }
            }
            return "";
        } finally {
            if (lock != null && lock.isHeld()) lock.release();
        }
    }

    private static String childText(org.w3c.dom.Element parent, String name) {
        NodeList nodes = parent.getElementsByTagName(name);
        return nodes.getLength() == 0 ? "" : nodes.item(0).getTextContent().trim();
    }

    private static byte[] soap(String endpoint, String action, String body) throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL(endpoint).openConnection();
        connection.setRequestMethod("POST");
        connection.setConnectTimeout(3000);
        connection.setReadTimeout(3000);
        connection.setDoOutput(true);
        connection.setRequestProperty("Content-Type", "text/xml; charset=\"utf-8\"");
        connection.setRequestProperty("SOAPAction", "\"" + action + "\"");
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        connection.setFixedLengthStreamingMode(bytes.length);
        OutputStream out = connection.getOutputStream();
        out.write(bytes);
        out.close();
        int code = connection.getResponseCode();
        InputStream in = code >= 200 && code < 300 ? connection.getInputStream() : connection.getErrorStream();
        if (in == null) { connection.disconnect(); return null; }
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        byte[] buffer = new byte[512];
        int count;
        while ((count = in.read(buffer)) != -1) result.write(buffer, 0, count);
        in.close();
        connection.disconnect();
        return code >= 200 && code < 300 ? result.toByteArray() : null;
    }

    static String encode(String address, boolean internet) {
        String[] octets = address.split("\\.");
        if (octets.length != 4) return "";
        long value = 0;
        try {
            for (String octet : octets) {
                if (octet.isEmpty()) return "";
                int number = Integer.parseInt(octet);
                if (number < 0 || number > 255 || !Integer.toString(number).equals(octet)) return "";
                value = (value << 8) | number;
            }
        } catch (NumberFormatException e) { return ""; }
        if (value == 0 || value >= 0xe0000000L) return "";
        if (internet) value += INTERNET_TAG;
        String result = "";
        for (int i = 0; i < 7; i++) {
            result = ALPHABET.charAt((int) (value % 36)) + result;
            value /= 36;
        }
        return result + check(result);
    }

    static String decode(String code) {
        if (code == null) return "";
        code = code.trim().toUpperCase(java.util.Locale.US);
        if (code.length() != 8 || code.charAt(7) != check(code.substring(0, 7))) return "";
        long value = 0;
        for (int i = 0; i < 7; i++) {
            int digit = ALPHABET.indexOf(code.charAt(i));
            if (digit < 0) return "";
            value = value * 36 + digit;
        }
        if (value >= INTERNET_TAG) value -= INTERNET_TAG;
        if (value <= 0 || value >= 0xe0000000L) return "";
        return ((value >> 24) & 255) + "." + ((value >> 16) & 255) + "."
                + ((value >> 8) & 255) + "." + (value & 255);
    }

    private static char check(String text) {
        int sum = 0;
        for (int i = 0; i < text.length(); i++) {
            int digit = ALPHABET.indexOf(text.charAt(i));
            if (digit < 0) return '?';
            sum = (sum * 17 + digit) % 36;
        }
        return ALPHABET.charAt(sum);
    }

    static String[] parseAddress(String input) {
        if (input == null) return null;
        String value = input.trim();
        String decoded = decode(value);
        if (!decoded.isEmpty()) return new String[] { decoded, Integer.toString(PORT) };
        String address = value;
        int port = PORT;
        int colon = value.lastIndexOf(':');
        if (colon > 0 && value.indexOf(':') == colon) {
            String suffix = value.substring(colon + 1);
            if (!suffix.matches("[0-9]{1,5}")) return null;
            port = Integer.parseInt(suffix);
            address = value.substring(0, colon);
        }
        if (port < 1 || port > 65535 || !address.matches("[0-9]{1,3}(\\.[0-9]{1,3}){3}")) return null;
        String[] octets = address.split("\\.");
        for (String octet : octets) {
            int number = Integer.parseInt(octet);
            if (number < 0 || number > 255) return null;
        }
        return new String[] { address, Integer.toString(port) };
    }
}
