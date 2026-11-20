import Foundation

/// Lekki log diagnostyczny — aktywny tylko przy zmiennej GPU_TRAY_DEBUG=1.
/// Zapisuje do ~/Library/Logs/GPUTray-debug.log (przydatne do weryfikacji pracy aplikacji).
enum DebugLog {
    static let enabled = ProcessInfo.processInfo.environment["GPU_TRAY_DEBUG"] != nil
    private static let url = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Logs/GPUTray-debug.log")

    static func log(_ message: String) {
        guard enabled else { return }
        let line = "\(Formatters.time.string(from: Date())) | \(message)\n"
        let dir = url.deletingLastPathComponent()
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        if let h = try? FileHandle(forWritingTo: url) {
            _ = try? h.seekToEnd()
            h.write(line.data(using: .utf8) ?? Data())
            try? h.close()
        } else {
            try? line.write(to: url, atomically: true, encoding: .utf8)
        }
    }
}
