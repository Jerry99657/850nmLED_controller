# 850nm 控制 App（Android）

手机本地运行的蓝牙遥控 App，界面就是 `tools/ble_control.html`，蓝牙走 Android 原生 API
（WebView 不支持 Web Bluetooth）。不依赖网络，也不需要 Gradle / Android Studio。

## 构建

```bat
python android/build.py
```

产物：`android/out/850nm-ble-controller-<版本>.apk`

脚本每次都会把 `tools/ble_control.html` 复制成 `assets/index.html`，所以改网页后重新跑一次即可，
浏览器版和 App 版界面始终一致。版本号写在 `build.py` 顶部的 `VERSION_NAME` / `VERSION_CODE`。

依赖的工具链（默认在 `D:/Tools/android`，可用环境变量 `ANDROID_TOOLS` 改）：

| 目录 | 内容 |
| --- | --- |
| `jdk-17/` | Temurin JDK 17（javac / keytool，d8、apksigner 需要 JDK ≥ 11） |
| `build-tools-34/` | aapt2 / d8 / zipalign / apksigner |
| `platform-34/` | android.jar（API 34） |

构建流程：`aapt2 compile` → `aapt2 link`（打包资源和 assets）→ `javac --release 8` →
`d8` → 把 `classes.dex` 塞进 APK → `zipalign` → `apksigner`。

> javac 用 `--release 8` 并把 android.jar 放在 `-classpath`。若改成
> `-bootclasspath android.jar`，会因 android.jar 里没有 `java.lang.invoke.LambdaMetafactory`
> 而编译不过。

## 安装

1. 把 `android/out/*.apk` 传到手机（微信/QQ 传文件、数据线、网盘都行）。
2. 打开文件，系统会提示"未知来源应用"，允许后安装。Android 12 及以上首次运行会申请
   "附近的设备"权限，必须允许，否则扫不到板子。
3. Android 11 及以下扫描 BLE 还要求打开手机的定位服务。

打开 App → 点扫描 → 列表里出现 `850nm-XXXX`（XXXX 是板子 MAC 后 4 位）→ 点它连接。

## 注意

- 签名密钥在 `android/keystore/release.p12`（口令 `850nm-ble`，别名 `nm850`，有效期 100 年）。
  升级 App 必须用同一个密钥签名，否则手机会拒绝覆盖安装、只能先卸载再装。这个密钥只用于自用侧载，
  不是应用商店密钥，丢了不影响安全，但**丢了就只能卸载重装**。文件不要提交到公开仓库。
- App 用 `WebView` 只加载本地 `assets/index.html`，所有外部链接都被拦掉，
  避免把 `NativeBle` 这个 JS 桥暴露给外部网页。
- `minSdkVersion 26`（Android 8.0）。
