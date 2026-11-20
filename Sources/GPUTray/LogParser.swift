import Foundation

/// Śledzi log llama-server (`~/.ollama/logs/server.log`), wyciągając szybkość generowania.
/// Przykładowa linia (llama.cpp):
/// `slot print_timing: id  0 | task 627 |  eval time = 55715.35 ms / 597 tokens (  93.48 ms per token, 10.70 tokens per second)`
final class OllamaLogParser: @unchecked Sendable {
    struct GenStat: Sendable {
        let tokPerSec: Double          // generowanie (eval)
        let promptTokPerSec: Double?   // prefill (prompt eval)
        let evalTokens: Int
        let evalMs: Double
        let date: Date                 // moment pojawienia się wpisu w logu
    }

    private let lock = NSLock()
    private var _lastStat: GenStat?
    var lastStat: GenStat? {
        lock.lock(); defer { lock.unlock() }
        return _lastStat
    }

    private let queue = DispatchQueue(label: "gpu-tray.logparser")
    private var timer: DispatchSourceTimer?
    private var fileURL: URL?
    private var offset: UInt64 = 0
    private var pendingLine = Data()
    private var pendingPrompt: Double?   // dostęp wyłącznie z kolejki `queue`

    /// Aktywny log: `server.log`, a gdy go brak — najnowszy `server-N.log` (rotacja).
    static func defaultLogURL() -> URL? {
        let dir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".ollama/logs")
        let main = dir.appendingPathComponent("server.log")
        let fm = FileManager.default
        if fm.fileExists(atPath: main.path) { return main }
        guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return nil }
        let rotated = names.filter { $0.hasPrefix("server-") && $0.hasSuffix(".log") }
        var best: URL?
        var bestDate = Date.distantPast
        for n in rotated {
            let u = dir.appendingPathComponent(n)
            let d = (try? fm.attributesOfItem(atPath: u.path))?[.modificationDate] as? Date ?? .distantPast
            if d > bestDate { bestDate = d; best = u }
        }
        return best
    }

    /// Skanuje ostatnie `chunkBytes` bajtów historii i ustawia `lastStat` na najnowszą generację.
    func scanHistory(chunkBytes: Int) -> GenStat? {
        guard let url = Self.defaultLogURL() else { return nil }
        let size = UInt64((try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? Int ?? 0)
        let start = size > UInt64(chunkBytes) ? size - UInt64(chunkBytes) : 0
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        h.seek(toFileOffset: start)
        let data = (try? h.read(upToCount: Int(size - start))) ?? Data()
        let text = String(data: data, encoding: .utf8) ?? ""

        var stat: GenStat?
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            if let s = parseLine(String(line), at: Date()) {
                stat = s
            }
        }
        if let stat { setStat(stat) }
        return stat
    }

    /// Wczytuje historię i rozpoczyna tailowanie pliku (poll co 1 s — odporne na rotację).
    func start() {
        if let url = Self.defaultLogURL() {
            let size = UInt64((try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? Int ?? 0)
            _ = scanHistory(chunkBytes: 256 * 1024)
            fileURL = url
            offset = size
        }
        queue.async { [weak self] in self?.schedule() }
    }

    private func schedule() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1.0, repeating: 1.0)
        t.setEventHandler { [weak self] in self?.timerTick() }
        t.resume()
        timer = t
    }

    private func timerTick() {
        if fileURL == nil {
            fileURL = Self.defaultLogURL()
            offset = 0
            pendingLine = Data()
            if fileURL == nil { return }
        }
        guard let url = fileURL,
              let attrs = try? FileManager.default.attributesOfItem(atPath: url.path) as [FileAttributeKey: Any] else {
            fileURL = nil
            return
        }
        let size = UInt64(attrs[.size] as? Int ?? 0)
        if size < offset {          // rotacja lub truncate
            offset = 0
            pendingLine = Data()
        }
        if size > offset {
            readRange(url: url, from: offset, to: size)
            offset = size
        }
    }

    private func readRange(url: URL, from: UInt64, to: UInt64) {
        guard let h = try? FileHandle(forReadingFrom: url) else { return }
        defer { try? h.close() }
        h.seek(toFileOffset: from)
        let len = Int(to - from)
        let data = (try? h.read(upToCount: len)) ?? Data()
        pendingLine.append(data)
        while let nl = pendingLine.firstIndex(of: UInt8(10)) {
            let lineData = pendingLine.prefix(nl)
            pendingLine = Data(pendingLine.dropFirst(nl + 1))
            if let line = String(data: lineData, encoding: .utf8) {
                if let s = parseLine(line, at: Date()) { setStat(s) }
            }
        }
    }

    private func setStat(_ s: GenStat) {
        lock.lock(); _lastStat = s; lock.unlock()
    }

    private static let regex = try! NSRegularExpression(
        pattern: "time\\s*=\\s*([0-9]+(?:\\.[0-9]+)?)\\s*ms\\s*/\\s*([0-9]+)\\s*tokens.*?([0-9]+(?:\\.[0-9]+)?)\\s*tokens per second"
    )

    private func parseLine(_ line: String, at date: Date) -> GenStat? {
        guard line.contains("tokens per second") else { return nil }
        guard let m = Self.regex.firstMatch(in: line, range: NSRange(line.startIndex..., in: line)) else { return nil }
        func group(_ i: Int) -> String {
            guard let r = Range(m.range(at: i), in: line) else { return "" }
            return String(line[r])
        }
        let ms = Double(group(1)) ?? 0
        let tokens = Int(group(2)) ?? 0
        let tps = Double(group(3)) ?? 0

        // "prompt eval time" → zapamiętaj jako prefill; faktyczna generacja to "eval time".
        if line.contains("prompt eval time") {
            pendingPrompt = tps
            return nil
        }
        let pp = pendingPrompt
        pendingPrompt = nil
        return GenStat(tokPerSec: tps, promptTokPerSec: pp, evalTokens: tokens, evalMs: ms, date: date)
    }
}
