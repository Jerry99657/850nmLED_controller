package com.nm850.ble;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.location.LocationManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/*
 * 850nm 控制板蓝牙遥控
 *
 * Android WebView 不支持 Web Bluetooth，因此界面沿用 assets/index.html（与 tools/ble_control.html 相同），
 * 蓝牙由本类用原生 API 实现，通过 window.NativeBle 暴露给网页：
 *   NativeBle.scan()              扫描 4 s，结果经 nativeBleEvent('device' / 'scanDone') 回调
 *   NativeBle.connect(address)    连接，订阅状态通知，经 nativeBleEvent('state' / 'status') 回调
 *   NativeBle.write("1,2,3,4")    写命令特征（逗号分隔的字节）
 *   NativeBle.disconnect()
 * 协议见固件 main/ble_link.h。所有蓝牙操作都在主线程中串行执行。
 */
public class MainActivity extends Activity {
    private static final UUID SVC = UUID.fromString("9e5d0001-3c1a-4b8e-9f2d-850e0a1c0001");
    private static final UUID CMD = UUID.fromString("9e5d0002-3c1a-4b8e-9f2d-850e0a1c0001");
    private static final UUID STA = UUID.fromString("9e5d0003-3c1a-4b8e-9f2d-850e0a1c0001");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final String NAME_PREFIX = "850nm-";
    private static final long SCAN_MS = 4000;
    private static final long CONNECT_TIMEOUT_MS = 10000;
    private static final int REQ_PERMS = 1;

    private final Handler main = new Handler(Looper.getMainLooper());
    private WebView web;
    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic cmdChr, staChr;
    private boolean scanning, ready;
    private Runnable pendingAction;
    private final Map<String, BluetoothDevice> found = new HashMap<>();
    private final Map<String, String> names = new HashMap<>();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.parseColor("#111111"));
        BluetoothManager bm = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = bm != null ? bm.getAdapter() : null;

        web = new WebView(this);
        web.setBackgroundColor(Color.parseColor("#111111"));
        WebSettings ws = web.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        // 只加载本地页面，外部链接一律不打开，避免把 NativeBle 暴露给外部网页
        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) {
                return true;
            }
        });
        web.addJavascriptInterface(new Bridge(), "NativeBle");
        setContentView(web);
        web.loadUrl("file:///android_asset/index.html");
    }

    @Override
    protected void onDestroy() {
        stopScan();
        closeGatt();
        web.destroy();
        super.onDestroy();
    }

    /* ---------- JS 接口（在 WebView 的 JavaBridge 线程调用，转到主线程执行） ---------- */

    private class Bridge {
        @JavascriptInterface
        public void scan() {
            main.post(() -> withPerms(MainActivity.this::startScan));
        }

        @JavascriptInterface
        public void connect(String address) {
            main.post(() -> withPerms(() -> doConnect(address)));
        }

        @JavascriptInterface
        public void write(String csv) {
            main.post(() -> doWrite(csv));
        }

        @JavascriptInterface
        public void disconnect() {
            main.post(() -> {
                closeGatt();
                emitState("disconnected", null);
            });
        }
    }

    /* ---------- 回调给 JS ---------- */

    private void emit(String type, String jsonData) {
        String js = "window.nativeBleEvent && window.nativeBleEvent(" + JSONObject.quote(type) + "," + jsonData + ")";
        main.post(() -> web.evaluateJavascript(js, null));
    }

    private void emitError(String msg) {
        emit("error", JSONObject.quote(msg));
    }

    private void emitState(String s, String name) {
        try {
            JSONObject o = new JSONObject();
            o.put("state", s);
            if (name != null) {
                o.put("name", name);
            }
            emit("state", o.toString());
        } catch (Exception ignored) {
        }
    }

    private void emitScanDone(boolean aborted) {
        emit("scanDone", aborted ? "{\"aborted\":true}" : "{}");
    }

    /* ---------- 权限与蓝牙开关 ---------- */

    private String[] neededPerms() {
        if (Build.VERSION.SDK_INT >= 31) {
            return new String[]{"android.permission.BLUETOOTH_SCAN", "android.permission.BLUETOOTH_CONNECT"};
        }
        return new String[]{Manifest.permission.ACCESS_FINE_LOCATION};
    }

    private void withPerms(Runnable action) {
        List<String> missing = new ArrayList<>();
        for (String p : neededPerms()) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) {
                missing.add(p);
            }
        }
        if (missing.isEmpty()) {
            action.run();
            return;
        }
        pendingAction = action;
        requestPermissions(missing.toArray(new String[0]), REQ_PERMS);
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        if (code != REQ_PERMS) {
            return;
        }
        boolean ok = results.length > 0;
        for (int r : results) {
            ok &= r == PackageManager.PERMISSION_GRANTED;
        }
        Runnable a = pendingAction;
        pendingAction = null;
        if (ok && a != null) {
            a.run();
        } else {
            emitError(Build.VERSION.SDK_INT >= 31 ? "需要“附近的设备”权限才能连接板子" : "需要“位置信息”权限才能扫描蓝牙");
            emitScanDone(true);
        }
    }

    private boolean bluetoothReady() {
        if (adapter == null) {
            emitError("这台手机不支持蓝牙");
            return false;
        }
        if (!adapter.isEnabled()) {
            emitError("请先打开手机蓝牙");
            try {
                startActivity(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE));
            } catch (SecurityException ignored) {
            }
            return false;
        }
        return true;
    }

    // Android 11 及以下关闭定位服务时扫描不到任何设备
    private boolean locationReady() {
        if (Build.VERSION.SDK_INT >= 31) {
            return true;
        }
        LocationManager lm = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        boolean on = lm != null && (Build.VERSION.SDK_INT >= 28 ? lm.isLocationEnabled()
                : lm.isProviderEnabled(LocationManager.GPS_PROVIDER) || lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER));
        if (!on) {
            emitError("请打开手机的定位服务（Android 11 及以下扫描蓝牙需要）");
        }
        return on;
    }

    /* ---------- 扫描 ---------- */

    private final Runnable scanTimeout = () -> {
        stopScan();
        emitScanDone(false);
    };

    private final ScanCallback scanCb = new ScanCallback() {
        @Override
        public void onScanResult(int type, ScanResult r) {
            main.post(() -> onFound(r));
        }

        @Override
        public void onScanFailed(int err) {
            main.post(() -> {
                stopScan();
                emitError("扫描失败（错误 " + err + "），请重试");
                emitScanDone(true);
            });
        }
    };

    private void startScan() {
        if (!bluetoothReady() || !locationReady()) {
            emitScanDone(true);
            return;
        }
        stopScan();
        found.clear();
        names.clear();
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            emitError("蓝牙未就绪，请重试");
            emitScanDone(true);
            return;
        }
        ScanSettings st = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        try {
            // 不用 UUID 过滤：服务 UUID 在扫描响应里，部分手机按 UUID 过滤会漏掉；改为按名称前缀筛选
            scanner.startScan(null, st, scanCb);
        } catch (SecurityException e) {
            emitError("没有蓝牙权限");
            emitScanDone(true);
            return;
        }
        scanning = true;
        main.postDelayed(scanTimeout, SCAN_MS);
    }

    private void stopScan() {
        main.removeCallbacks(scanTimeout);
        if (scanning && scanner != null) {
            try {
                scanner.stopScan(scanCb);
            } catch (RuntimeException ignored) {
            }
        }
        scanning = false;
    }

    private void onFound(ScanResult r) {
        if (!scanning) {
            return;
        }
        BluetoothDevice d = r.getDevice();
        String name = r.getScanRecord() != null ? r.getScanRecord().getDeviceName() : null;
        if (name == null || !name.startsWith(NAME_PREFIX) || found.containsKey(d.getAddress())) {
            return;
        }
        found.put(d.getAddress(), d);
        names.put(d.getAddress(), name);
        try {
            JSONObject o = new JSONObject();
            o.put("name", name);
            o.put("address", d.getAddress());
            o.put("rssi", r.getRssi());
            emit("device", o.toString());
        } catch (Exception ignored) {
        }
    }

    /* ---------- 连接 ---------- */

    private final Runnable connectTimeout = () -> {
        if (!ready) {
            closeGatt();
            emitError("连接超时，请靠近板子后重试");
            emitState("disconnected", null);
        }
    };

    private void doConnect(String address) {
        stopScan();
        closeGatt();
        if (!bluetoothReady()) {
            emitState("disconnected", null);
            return;
        }
        BluetoothDevice d = found.get(address);
        if (d == null) {
            try {
                d = adapter.getRemoteDevice(address);
            } catch (IllegalArgumentException e) {
                emitError("设备地址无效");
                emitState("disconnected", null);
                return;
            }
        }
        String name = names.containsKey(address) ? names.get(address) : address;
        emitState("connecting", name);
        try {
            gatt = d.connectGatt(this, false, gattCb, BluetoothDevice.TRANSPORT_LE);
        } catch (SecurityException e) {
            emitError("没有蓝牙权限");
            emitState("disconnected", null);
            return;
        }
        main.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS);
    }

    private void closeGatt() {
        main.removeCallbacks(connectTimeout);
        if (gatt != null) {
            try {
                gatt.disconnect();
                gatt.close();
            } catch (SecurityException ignored) {
            }
        }
        gatt = null;
        cmdChr = null;
        staChr = null;
        ready = false;
    }

    private final BluetoothGattCallback gattCb = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            main.post(() -> {
                if (g != gatt) {
                    return;
                }
                if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) {
                    try {
                        g.discoverServices();
                    } catch (SecurityException e) {
                        fail("没有蓝牙权限");
                    }
                } else {
                    boolean wasReady = ready;
                    closeGatt();
                    if (!wasReady) {
                        emitError("连接失败（状态 " + status + "），请重试");
                    }
                    emitState("disconnected", null);
                }
            });
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            main.post(() -> {
                if (g != gatt) {
                    return;
                }
                BluetoothGattService s = g.getService(SVC);
                if (status != BluetoothGatt.GATT_SUCCESS || s == null) {
                    fail("该设备不是 850nm 控制板，或固件不支持蓝牙控制");
                    return;
                }
                cmdChr = s.getCharacteristic(CMD);
                staChr = s.getCharacteristic(STA);
                BluetoothGattDescriptor cccd = staChr != null ? staChr.getDescriptor(CCCD) : null;
                if (cmdChr == null || cccd == null) {
                    fail("板子的蓝牙服务不完整");
                    return;
                }
                try {
                    g.setCharacteristicNotification(staChr, true);
                    byte[] v = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE;
                    boolean ok;
                    if (Build.VERSION.SDK_INT >= 33) {
                        ok = g.writeDescriptor(cccd, v) == BluetoothStatusCodes.SUCCESS;
                    } else {
                        cccd.setValue(v);
                        ok = g.writeDescriptor(cccd);
                    }
                    if (!ok) {
                        fail("订阅状态失败");
                    }
                } catch (SecurityException e) {
                    fail("没有蓝牙权限");
                }
            });
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            main.post(() -> {
                if (g != gatt) {
                    return;
                }
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    fail("订阅状态失败");
                    return;
                }
                try {
                    if (!g.readCharacteristic(staChr)) {
                        fail("读取状态失败");
                    }
                } catch (SecurityException e) {
                    fail("没有蓝牙权限");
                }
            });
        }

        // Android 13 以上走带 value 的重载；以下走旧重载。两者只处理其一，避免重复
        @Override
        public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value, int status) {
            onRead(g, value, status);
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicRead(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            if (Build.VERSION.SDK_INT < 33) {
                byte[] v = c.getValue();
                onRead(g, v != null ? v.clone() : null, status);
            }
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            onStatus(g, value);
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (Build.VERSION.SDK_INT < 33) {
                byte[] v = c.getValue();
                onStatus(g, v != null ? v.clone() : null);
            }
        }
    };

    private void onRead(BluetoothGatt g, byte[] value, int status) {
        main.post(() -> {
            if (g != gatt) {
                return;
            }
            if (status != BluetoothGatt.GATT_SUCCESS || value == null) {
                fail("读取状态失败");
                return;
            }
            // 先送出初始状态（JS 据此同步 seq），再报告已连接
            sendStatus(value);
            main.removeCallbacks(connectTimeout);
            ready = true;
            BluetoothDevice d = g.getDevice();
            String a = d.getAddress();
            emitState("connected", names.containsKey(a) ? names.get(a) : a);
        });
    }

    private void onStatus(BluetoothGatt g, byte[] value) {
        main.post(() -> {
            if (g == gatt && value != null) {
                sendStatus(value);
            }
        });
    }

    private void sendStatus(byte[] v) {
        StringBuilder sb = new StringBuilder("[");
        for (int i = 0; i < v.length; i++) {
            if (i > 0) {
                sb.append(',');
            }
            sb.append(v[i] & 0xFF);
        }
        emit("status", sb.append(']').toString());
    }

    private void fail(String msg) {
        closeGatt();
        emitError(msg);
        emitState("disconnected", null);
    }

    /* ---------- 写命令 ---------- */

    private void doWrite(String csv) {
        if (!ready || gatt == null || cmdChr == null) {
            emitError("未连接");
            return;
        }
        byte[] buf;
        try {
            String[] parts = csv.split(",");
            buf = new byte[parts.length];
            for (int i = 0; i < parts.length; i++) {
                buf[i] = (byte) Integer.parseInt(parts[i].trim());
            }
        } catch (NumberFormatException e) {
            emitError("命令格式错误");
            return;
        }
        try {
            boolean ok;
            if (Build.VERSION.SDK_INT >= 33) {
                ok = gatt.writeCharacteristic(cmdChr, buf, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                        == BluetoothStatusCodes.SUCCESS;
            } else {
                cmdChr.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                cmdChr.setValue(buf);
                ok = gatt.writeCharacteristic(cmdChr);
            }
            if (!ok) {
                emitError("发送失败，请重试");
            }
        } catch (SecurityException e) {
            emitError("没有蓝牙权限");
        }
    }
}
