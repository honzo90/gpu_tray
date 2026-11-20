import AppKit

// Tryb diagnostyczny: `gpu-tray --once` — wypisuje jeden snapshot danych i kończy.
if CommandLine.arguments.contains("--once") {
    Diagnostics.runOnce()
    exit(0)
}

// Tryb testowy ekranu ESP32: `gpu-tray --screen-test` — odpowiada na CYDREQ danymi
// syntetycznymi (do konfiguracji Wi-Fi/kalibracji na samym CYD, bez GUI).
if CommandLine.arguments.contains("--screen-test") {
    ScreenTest.run()
    exit(0)
}

// Zabezpieczenie: tylko jedna instancja aplikacji.
if let bid = Bundle.main.infoDictionary?["CFBundleIdentifier"] as? String, !bid.isEmpty {
    let others = NSRunningApplication.runningApplications(withBundleIdentifier: bid)
        .filter { $0.processIdentifier != ProcessInfo.processInfo.processIdentifier }
    if !others.isEmpty {
        exit(0)
    }
}

UserDefaults.standard.register(defaults: [
    "gpuBusyThreshold": 60.0,
])

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var tray: StatusBarController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        tray = StatusBarController()
        Notifier.requestAuthorizationIfNeeded()
        DebugLog.log("GPU Tray uruchomiony (\(Bundle.main.bundlePath))")
    }
}
