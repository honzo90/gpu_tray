import Foundation

/// Model załadowany w Ollama (odpowiedź `GET /api/ps`).
struct RunningModel: Codable {
    struct Details: Codable {
        let parameterSize: String?
        enum CodingKeys: String, CodingKey { case parameterSize = "parameter_size" }
    }

    let name: String
    let size: Int64
    let sizeVram: Int64
    let expiresAt: Date?
    let contextLength: Int?
    let details: Details?

    enum CodingKeys: String, CodingKey {
        case name, size, details
        case sizeVram = "size_vram"
        case expiresAt = "expires_at"
        case contextLength = "context_length"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        size = try c.decodeIfPresent(Int64.self, forKey: .size) ?? 0
        sizeVram = try c.decodeIfPresent(Int64.self, forKey: .sizeVram) ?? 0
        contextLength = try c.decodeIfPresent(Int.self, forKey: .contextLength)
        details = try c.decodeIfPresent(Details.self, forKey: .details)
        if let s = try c.decodeIfPresent(String.self, forKey: .expiresAt) {
            expiresAt = Self.parseDate(s)
        } else {
            expiresAt = nil
        }
    }

    /// Tolerancyjny parser ISO8601: "2026-11-20T16:43:09.822592+01:00".
    /// Rok 0001 oznacza brak wygaśnięcia (keep_alive = -1).
    static func parseDate(_ s: String) -> Date? {
        let frac = ISO8601DateFormatter()
        frac.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let d = frac.date(from: s), d.timeIntervalSince1970 > 0 {
            return d
        }
        var plain = s
        if let r = plain.range(of: "\\.[0-9]+", options: .regularExpression) {
            plain.replaceSubrange(r, with: "")
        }
        let fmt = ISO8601DateFormatter()
        if let d = fmt.date(from: plain), d.timeIntervalSince1970 > 0 {
            return d
        }
        return nil
    }

    /// Krótka nazwa: "hf.co/X/Qwen3.8-27B-GGUF:Q4_K_M" → "Qwen3.8-27B-GGUF".
    var shortName: String {
        let base = name.split(separator: "/").last.map(String.init) ?? name
        return base.split(separator: ":").first.map(String.init) ?? base
    }

    /// Ile procent modelu siedzi w VRAM/GPU.
    var gpuPercent: Int {
        guard size > 0 else { return 0 }
        return Int((Double(sizeVram) / Double(size)) * 100)
    }
}

struct PSResponse: Codable {
    let models: [RunningModel]?
}

/// Klient HTTP serwera Ollama z auto-detekcją adresu.
/// Uwaga: Ollama z OLLAMA_HOST=0.0.0.0 lub IP LAN (np. http://192.168.1.50:11434)
/// nie musi odpowiadać na localhost — dlatego probe'ujemy kilka kandydatów.
final class OllamaClient: @unchecked Sendable {
    private let lock = NSLock()
    private var _baseURL: URL?
    private var lastProbe = Date.distantPast
    private let session: URLSession

    var baseURL: URL? {
        lock.lock(); defer { lock.unlock() }
        return _baseURL
    }

    init() {
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 2
        cfg.timeoutIntervalForResource = 5
        session = URLSession(configuration: cfg)
    }

    private static var port: Int {
        let p = UserDefaults.standard.integer(forKey: "ollamaPort")
        return p > 0 ? p : 11434
    }

    /// Kandydackie adresy: UserDefaults → 127.0.0.1 → adresy nasłuchu z lsof.
    static func candidateURLs() -> [URL] {
        var urls: [URL] = []
        if let saved = UserDefaults.standard.string(forKey: "ollamaBaseURL"),
           let u = URL(string: saved) {
            urls.append(u)
        }
        let p = port
        if let loop = URL(string: "http://127.0.0.1:\(p)") { urls.append(loop) }
        if let lh = URL(string: "http://localhost:\(p)") { urls.append(lh) }
        for ip in listenAddresses(port: p) {
            if let u = URL(string: "http://\(ip):\(p)") { urls.append(u) }
        }
        var seen = Set<URL>()
        return urls.filter { seen.insert($0).inserted }
    }

    /// Adresy IPv4 nasłuchujące na porcie Ollama — odczyt z lsof (bez roota, procesy tego samego usera).
    static func listenAddresses(port: Int) -> [String] {
        let paths = ["/usr/sbin/lsof", "/usr/bin/lsof"]
        guard let lsof = paths.first(where: { FileManager.default.fileExists(atPath: $0) }) else { return [] }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: lsof)
        p.arguments = ["-iTCP:\(port)", "-sTCP:LISTEN", "-P", "-n"]
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = FileHandle.nullDevice
        do {
            try p.run()
            p.waitUntilExit()
        } catch { return [] }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        guard let out = String(data: data, encoding: .utf8) else { return [] }

        var addrs: [String] = []
        let re = try? NSRegularExpression(pattern: "([0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}):[0-9]+")
        for line in out.split(separator: "\n") {
            let s = String(line)
            if let m = re?.firstMatch(in: s, range: NSRange(s.startIndex..., in: s)),
               let r = Range(m.range(at: 1), in: s) {
                let a = String(s[r])
                if a != "127.0.0.1" && !addrs.contains(a) { addrs.append(a) }
            }
        }
        return addrs
    }

    /// Synchroniczne pomocniki stanu (NSLock niedostępny w kontekście async).
    private func markProbed() {
        lock.lock(); lastProbe = Date(); lock.unlock()
    }

    private func secondsSinceProbe() -> TimeInterval {
        lock.lock(); defer { lock.unlock() }
        return Date().timeIntervalSince(lastProbe)
    }

    private func setBaseURL(_ url: URL?) {
        lock.lock(); _baseURL = url; lock.unlock()
    }

    /// Próbuje znaleźć działający serwer. Wywoływane przy starcie i przy braku odpowiedzi.
    func refreshBaseURL(force: Bool = false) async {
        let have = baseURL != nil
        markProbed()
        if have && !force && secondsSinceProbe() < 30 { return }

        let candidates = await Task.detached(priority: .utility) { Self.candidateURLs() }.value
        DebugLog.log("Ollama: probe kandydatów (\(candidates.count) szt.)")
        for url in candidates {
            var req = URLRequest(url: url.appendingPathComponent("api/version"))
            req.timeoutInterval = 1.5
            do {
                let (_, resp) = try await session.data(for: req)
                if let http = resp as? HTTPURLResponse, (200..<300).contains(http.statusCode) {
                    setBaseURL(url)
                    DebugLog.log("Ollama: serwer wykryty — \(url.absoluteString)")
                    return
                }
            } catch {
                DebugLog.log("Ollama: \(url.absoluteString) → \(error.localizedDescription)")
            }
        }
        DebugLog.log("Ollama: nie znaleziono serwera")
    }

    /// Załadowane modele; nil = serwer nieosiągalny.
    func fetchRunningModels() async -> [RunningModel]? {
        guard let base = baseURL else { return nil }
        var req = URLRequest(url: base.appendingPathComponent("api/ps"))
        req.timeoutInterval = 3
        do {
            let (data, resp) = try await session.data(for: req)
            guard let http = resp as? HTTPURLResponse, (200..<300).contains(http.statusCode) else { return nil }
            return try JSONDecoder().decode(PSResponse.self, from: data).models
        } catch {
            return nil
        }
    }

    /// Zwalnia model z VRAM — oficjalny sposób: POST /api/generate z keep_alive = 0.
    func unloadModel(_ name: String) async -> Bool {
        guard let base = baseURL else { return false }
        var req = URLRequest(url: base.appendingPathComponent("api/generate"))
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONSerialization.data(withJSONObject: ["model": name, "keep_alive": 0])
        req.timeoutInterval = 15
        do {
            let (_, resp) = try await session.data(for: req)
            let ok = (resp as? HTTPURLResponse).map { (200..<300).contains($0.statusCode) } ?? false
            DebugLog.log("Unload \(name): \(ok ? "OK" : "błąd")")
            return ok
        } catch {
            DebugLog.log("Unload \(name): \(error.localizedDescription)")
            return false
        }
    }
}
