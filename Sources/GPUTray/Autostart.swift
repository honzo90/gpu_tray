import Foundation

/// Autostart przez LaunchAgent (~/Library/LaunchAgents/com.gputray.agent.plist).
/// Agent uruchamia `open -a GPUTray.app` przy logowaniu (RunAtLoad).
enum Autostart {
    static let label = "com.gputray.agent"

    static var plistURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }

    static var isEnabled: Bool {
        FileManager.default.fileExists(atPath: plistURL.path)
    }

    static func setEnabled(_ enabled: Bool) {
        if enabled {
            guard Bundle.main.infoDictionary?["CFBundleIdentifier"] as? String != nil else { return }
            let appPath = Bundle.main.bundlePath
            let dict: [String: Any] = [
                "Label": label,
                "ProgramArguments": ["/usr/bin/open", "-a", appPath],
                "RunAtLoad": true,
            ]
            do {
                let dir = plistURL.deletingLastPathComponent()
                try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                let data = try PropertyListSerialization.data(fromPropertyList: dict, format: .xml, options: 0)
                try data.write(to: plistURL)
                DebugLog.log("Autostart: włączony (\(plistURL.path))")
            } catch {
                DebugLog.log("Autostart: błąd zapisu — \(error.localizedDescription)")
            }
        } else {
            try? FileManager.default.removeItem(at: plistURL)
            DebugLog.log("Autostart: wyłączony")
        }
    }
}
