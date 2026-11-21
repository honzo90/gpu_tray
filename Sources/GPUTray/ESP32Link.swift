import Foundation
import Darwin

/// Łącze z ekranikiem ESP32 (CYD) po UDP — port 45678.
/// ESP32 rozgłasza "CYDREQ\n" (broadcast), dopóki nie pozna IP Maca; na każdy REQ
/// odpowiadamy jednym JSON-em na adres i port nadawcy. Zero konfiguracji IP po
/// obu stronach; łącze samo się leczy po restarcie Maca, routera czy ESP32.
///
/// Gwóźdź: gniazdo BSD (recvfrom/sendto) — bez NWConnection/ATS; działa też
/// w "gołym" binarium z `--screen-test`.
final class ESP32Link: @unchecked Sendable {
    /// Migawka danych do wysłania na ekran.
    struct Snapshot {
        var host = ProcessInfo.processInfo.hostName
        var time = ""                 // "HH:mm:ss" — zegar na ekranie
        var gpu: Double?              // Device Utilization %
        var cpu: Double?              // %
        var memUsedGB: Double?        // RAM
        var memTotalGB: Double?
        var vgpuGB: Double?           // pamięć GPU "in use"
        var netDownMB = 0.0
        var netUpMB = 0.0
        var busy = false              // Ollama generuje
        var genSeconds: Int?          // czas bieżącej generacji
        var model: String?            // krótka nazwa modelu
        var tokPerSec: Double?        // ostatnia ukończona generacja
        var tokAge: Int?              // s od tej generacji
        /// Drukarka Bambu Lab: nil = nieskonfigurowana (CYD pokazuje podpowiedź),
        /// printerLinked = jest połączenie z drukarką.
        var printer: PrinterState?
        var printerConfigured = false
        var printerLinked = false
    }

    static let port: UInt16 = 45678

    private let lock = NSLock()
    private var snapshot: Snapshot?
    private var peer: (ip: String, at: Date)?
    private var fd: Int32 = -1

    /// Log każdego REQ (używa `--screen-test`).
    var onRequest: (@Sendable (String) -> Void)?

    /// Ustawia dane, które polecą przy najbliższym REQ. Wywoływane z tickSecond.
    func publish(_ s: Snapshot) {
        lock.lock()
        snapshot = s
        lock.unlock()
    }

    /// Etykieta stanu dla menu: nil gdy ekran jeszcze o nic nie pytał.
    func peerLabel(now: Date = Date()) -> String? {
        lock.lock()
        defer { lock.unlock() }
        guard let p = peer else { return nil }
        let age = now.timeIntervalSince(p.at)
        return age < 15 ? "podłączony (\(p.ip))" : "brak REQ od \(p.ip) (\(Int(age)) s)"
    }

    /// Startuje nasłuch (raz, przy starcie aplikacji).
    func start() {
        fd = Darwin.socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { return }
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = Self.port.bigEndian
        let bound = withUnsafePointer(to: &addr) { p -> Int32 in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0 else {
            DebugLog.log("ESP32Link: bind UDP \(Self.port) nieudany (\(String(cString: strerror(errno))))")
            return
        }
        DebugLog.log("ESP32Link: nasłuch UDP \(Self.port) — czekam na CYDREQ")
        DispatchQueue(label: "gpu-tray.esp32link.rx", qos: .utility).async { [weak self] in
            guard let self else { return }
            self.receiveLoop()
        }
    }

    // MARK: - Odbiór / odpowiedź

    private func receiveLoop() {
        var buf = [UInt8](repeating: 0, count: 128)
        while true {
            var from = sockaddr_in()
            var fromLen = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = withUnsafeMutablePointer(to: &from) { fp -> Int in
                fp.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    buf.withUnsafeMutableBytes { raw in
                        recvfrom(fd, raw.baseAddress, raw.count, 0, sa, &fromLen)
                    }
                }
            }
            if n <= 0 { usleep(200_000); continue }
            guard String(bytes: buf.prefix(n), encoding: .utf8)?.hasPrefix("CYDREQ") == true else { continue }

            let ip = Self.ntop(from)
            lock.lock()
            peer = (ip, Date())
            lock.unlock()
            onRequest?(ip)
            if let data = snapshotJSON() {
                reply(data, to: from)
            }
        }
    }

    private func reply(_ data: Data, to from: sockaddr_in) {
        var to = from
        data.withUnsafeBytes { (p: UnsafeRawBufferPointer) -> Void in
            withUnsafePointer(to: &to) { fp in
                fp.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    _ = Darwin.sendto(fd, p.baseAddress, p.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    // MARK: - JSON

    private func snapshotJSON() -> Data? {
        lock.lock()
        let s = snapshot
        lock.unlock()
        guard let s else { return nil }

        var host = s.host
        if host.hasSuffix(".local") { host.removeLast(6) }
        var o: [String: Any] = [
            "h": host,
            "t": s.time,
            "busy": s.busy ? 1 : 0,
        ]
        // NaN/inf → JSONSerialization odrzuca cały pakiet, więc takie pola pomijamy;
        // reszta zaokrąglona (krótszy pakiet). Sieć z dokładnością 0.01 MB/s.
        func put(_ k: String, _ v: Double?, _ scale: Double = 10) {
            guard let v, v.isFinite else { return }
            o[k] = (v * scale).rounded() / scale
        }
        put("nd", s.netDownMB, 100)
        put("nu", s.netUpMB, 100)
        put("gpu", s.gpu)
        put("cpu", s.cpu)
        if let u = s.memUsedGB, let t = s.memTotalGB, u.isFinite, t.isFinite {
            put("mem", u, 100); put("memT", t, 100)
        }
        put("vgpu", s.vgpuGB, 100)
        if s.busy {
            if let m = s.model { o["model"] = m }
            if let g = s.genSeconds { o["gen"] = g }
        }
        put("tok", s.tokPerSec)
        if let v = s.tokAge { o["tokA"] = v }
        if s.printerConfigured { o["p"] = printerJSON(s) }
        return try? JSONSerialization.data(withJSONObject: o)
    }

    /// Zwarty obiekt drukarki (~250 B). Teksty w ASCII — fonty CYD nie mają diakrytyków.
    private func printerJSON(_ s: Snapshot) -> [String: Any] {
        var p: [String: Any] = ["ok": s.printerLinked ? 1 : 0]
        guard let st = s.printer, st.updated != .distantPast else { p["age"] = -1; return p }
        p["age"] = Int(Date().timeIntervalSince(st.updated))
        func put(_ k: String, _ v: Double?) {
            guard let v, v.isFinite else { return }
            p[k] = (v * 10).rounded() / 10
        }
        if let v = st.state { p["st"] = v }
        if let v = st.percent { p["pct"] = v }
        if let v = st.remainingMin { p["rem"] = v }
        if let v = st.layer { p["ly"] = v }
        if let v = st.totalLayers { p["lyT"] = v }
        if let f = st.file { p["f"] = Self.ascii(f, max: 40) }
        put("nz", st.nozzle); put("nzT", st.nozzleTarget)
        put("bd", st.bed); put("bdT", st.bedTarget)
        put("ch", st.chamber)
        if let v = st.filamentType { p["fil"] = Self.ascii(v, max: 10) }
        if let v = st.filamentColor { p["col"] = v }
        if let v = st.filamentRemain { p["fr"] = v }
        if !st.amsColors.isEmpty { p["ams"] = st.amsColors.joined(separator: ",") }
        if let v = st.activeSlot { p["act"] = v }
        if let v = st.error, v != 0 { p["err"] = v }
        return p
    }

    /// „Żółta łódź.3mf" → „Zolta lodz.3mf" (transliteracja + tylko drukowalne ASCII).
    static func ascii(_ s: String, max: Int) -> String {
        let latin = s.applyingTransform(.toLatin, reverse: false) ?? s
        let plain = latin.applyingTransform(.stripDiacritics, reverse: false) ?? latin
        let mapped = plain.replacingOccurrences(of: "ł", with: "l").replacingOccurrences(of: "Ł", with: "L")
        let out = String(String.UnicodeScalarView(mapped.unicodeScalars.filter { $0.value >= 0x20 && $0.value < 0x7F }))
        return String(out.prefix(max))
    }

    private static func ntop(_ sa: sockaddr_in) -> String {
        var a = sa.sin_addr
        var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &a, &buf, socklen_t(INET_ADDRSTRLEN))
        return String(cString: buf)
    }
}
