import Foundation
import Network

/// Stan druku wyciągnięty z raportów drukarki Bambu Lab (pola obiektu "print").
struct PrinterState {
    var state: String?          // gcode_state: IDLE / PREPARE / RUNNING / PAUSE / FINISH / FAILED
    var percent: Int?           // mc_percent
    var remainingMin: Int?      // mc_remaining_time (minuty)
    var layer: Int?             // layer_num
    var totalLayers: Int?       // total_layer_num
    var file: String?           // subtask_name (lub gcode_file)
    var nozzle: Double?, nozzleTarget: Double?
    var bed: Double?, bedTarget: Double?
    var chamber: Double?
    var filamentType: String?   // aktywny slot AMS / szpula zewnętrzna
    var filamentColor: String?  // "RRGGBB"
    var filamentRemain: Int?    // % filamentu na aktywnej szpuli (gdy AMS zna)
    var amsColors: [String] = []    // kolory slotów pierwszego AMS ("RRGGBB", "" = pusty)
    var activeSlot: Int?            // indeks w amsColors (aktywny lub ostatnio użyty)
    var error: Int?             // print_error (0 = brak)
    var updated = Date.distantPast
}

/// Minimalny klient MQTT 3.1.1 (TLS, port 8883) do lokalnego API drukarek Bambu Lab.
/// Logowanie: użytkownik "bblp", hasło = kod dostępu LAN z ekranu drukarki.
/// Subskrybuje `device/<serial>/report`, po połączeniu prosi raz o pełny stan (`pushall`),
/// potem drukarka wysyła tylko zmiany — scalamy je w jeden słownik.
///
/// Certyfikat drukarki jest samopodpisany (CA Bambu) — przyjmujemy go bez weryfikacji;
/// połączenie idzie wyłącznie po sieci lokalnej do adresu wpisanego przez użytkownika.
final class BambuClient: @unchecked Sendable {
    struct Config: Equatable {
        var host: String
        var serial: String
        var accessCode: String

        /// Z UserDefaults: bambuHost / bambuSerial / bambuAccessCode; nil gdy niepełne.
        static func fromDefaults() -> Config? {
            let d = UserDefaults.standard
            guard let h = d.string(forKey: "bambuHost"), !h.isEmpty,
                  let s = d.string(forKey: "bambuSerial"), !s.isEmpty,
                  let c = d.string(forKey: "bambuAccessCode"), !c.isEmpty else { return nil }
            return Config(host: h.trimmingCharacters(in: .whitespaces),
                          serial: s.trimmingCharacters(in: .whitespaces),
                          accessCode: c.trimmingCharacters(in: .whitespaces))
        }
    }

    enum Link: Equatable { case off, connecting, connected, failed(String) }

    let config: Config
    /// Surowe raporty (tryb --bambu-dump).
    var onRawReport: ((String) -> Void)?
    /// Zmiana stanu połączenia (log / --bambu-dump).
    var onLink: ((Link) -> Void)?

    private let queue = DispatchQueue(label: "gpu-tray.bambu", qos: .utility)
    private let lock = NSLock()
    private var conn: NWConnection?
    private var rx = Data()
    private var merged: [String: Any] = [:]
    private var _state = PrinterState()
    private var _link: Link = .off
    private var pingTimer: DispatchSourceTimer?
    private var backoff: TimeInterval = 5
    private var stopped = false
    private var packetId: UInt16 = 1

    init(config: Config) { self.config = config }

    var state: PrinterState { lock.lock(); defer { lock.unlock() }; return _state }
    var link: Link { lock.lock(); defer { lock.unlock() }; return _link }

    func start() {
        queue.async { self.stopped = false; self.connect() }
    }

    func stop() {
        queue.async {
            self.stopped = true
            self.teardown()
            self.setLink(.off)
        }
    }

    // MARK: - Połączenie

    private func connect() {
        guard !stopped else { return }
        teardown()
        rx.removeAll()
        setLink(.connecting)

        let tls = NWProtocolTLS.Options()
        sec_protocol_options_set_verify_block(tls.securityProtocolOptions, { _, _, done in done(true) }, queue)
        let tcp = NWProtocolTCP.Options()
        tcp.connectionTimeout = 10
        let params = NWParameters(tls: tls, tcp: tcp)
        let c = NWConnection(host: NWEndpoint.Host(config.host), port: 8883, using: params)
        conn = c
        c.stateUpdateHandler = { [weak self] st in
            guard let self else { return }
            switch st {
            case .ready:
                self.send(self.connectPacket())
                self.receive()
            case .failed(let e):
                self.fail("połączenie: \(e)")
            case .waiting(let e):
                self.fail("czekam na sieć: \(e)")
            default: break
            }
        }
        c.start(queue: queue)
    }

    private func teardown() {
        pingTimer?.cancel()
        pingTimer = nil
        conn?.stateUpdateHandler = nil
        conn?.cancel()
        conn = nil
    }

    private func fail(_ why: String) {
        guard !stopped else { return }
        DebugLog.log("Bambu: \(why) — ponowna próba za \(Int(backoff)) s")
        setLink(.failed(why))
        teardown()
        let delay = backoff
        backoff = min(backoff * 2, 60)
        queue.asyncAfter(deadline: .now() + delay) { [weak self] in self?.connect() }
    }

    private func setLink(_ l: Link) {
        lock.lock(); _link = l; lock.unlock()
        onLink?(l)
    }

    private func send(_ d: Data) {
        conn?.send(content: d, completion: .contentProcessed { [weak self] e in
            if let e { self?.queue.async { self?.fail("wysyłanie: \(e)") } }
        })
    }

    private func receive() {
        conn?.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, done, err in
            guard let self else { return }
            if let data, !data.isEmpty { self.rx.append(data); self.drain() }
            if let err { self.fail("odbiór: \(err)"); return }
            if done { self.fail("drukarka zamknęła połączenie"); return }
            self.receive()
        }
    }

    // MARK: - MQTT: pakiety

    private static func str(_ s: String) -> Data {
        let u = Data(s.utf8)
        return Data([UInt8(u.count >> 8), UInt8(u.count & 0xFF)]) + u
    }

    private static func remLen(_ n: Int) -> Data {
        var n = n, out = Data()
        repeat {
            var b = UInt8(n % 128); n /= 128
            if n > 0 { b |= 0x80 }
            out.append(b)
        } while n > 0
        return out
    }

    private static func packet(_ type: UInt8, _ body: Data) -> Data {
        Data([type]) + remLen(body.count) + body
    }

    private func connectPacket() -> Data {
        var v = Self.str("MQTT")
        v.append(4)                     // MQTT 3.1.1
        v.append(0xC2)                  // username + password + clean session
        v.append(contentsOf: [0, 60])   // keepalive 60 s
        let id = "gputray-\(UInt32.random(in: 0...UInt32.max))"
        let body = v + Self.str(id) + Self.str("bblp") + Self.str(config.accessCode)
        return Self.packet(0x10, body)
    }

    private func subscribePacket(_ topic: String) -> Data {
        packetId &+= 1
        let body = Data([UInt8(packetId >> 8), UInt8(packetId & 0xFF)]) + Self.str(topic) + Data([0])
        return Self.packet(0x82, body)
    }

    private func publishPacket(_ topic: String, _ payload: String) -> Data {
        Self.packet(0x30, Self.str(topic) + Data(payload.utf8))
    }

    private func startPing() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 30, repeating: 30)
        t.setEventHandler { [weak self] in self?.send(Data([0xC0, 0x00])) }
        t.resume()
        pingTimer = t
    }

    // MARK: - MQTT: odbiór

    /// Wyjmuje z bufora kompletne pakiety.
    private func drain() {
        while true {
            guard rx.count >= 2 else { return }
            let bytes = [UInt8](rx.prefix(5))
            var len = 0, mul = 1, i = 1
            while true {
                guard i < bytes.count else { return }       // długość jeszcze niekompletna
                len += Int(bytes[i] & 0x7F) * mul
                if bytes[i] & 0x80 == 0 { break }
                mul *= 128; i += 1
                if i > 4 { fail("zły pakiet MQTT"); return }
            }
            let total = 1 + i + len
            guard rx.count >= total else { return }
            let type = bytes[0]
            let body = rx.subdata(in: rx.startIndex + 1 + i ..< rx.startIndex + total)
            rx.removeSubrange(rx.startIndex ..< rx.startIndex + total)
            handle(type: type, body: body)
        }
    }

    private func handle(type: UInt8, body: Data) {
        let b = [UInt8](body)
        switch type & 0xF0 {
        case 0x20:   // CONNACK
            let rc = b.count >= 2 ? b[1] : 255
            guard rc == 0 else {
                let why = rc == 4 || rc == 5
                    ? "odmowa (kod \(rc)) — sprawdź kod dostępu; w nowszym firmware włącz tryb LAN / Developer Mode"
                    : "CONNACK \(rc)"
                stopped = rc == 4 || rc == 5        // zły kod → nie męczymy drukarki co minutę
                setLink(.failed(why))
                DebugLog.log("Bambu: \(why)")
                teardown()
                return
            }
            backoff = 5
            setLink(.connected)
            DebugLog.log("Bambu: połączono z \(config.host)")
            send(subscribePacket("device/\(config.serial)/report"))
            send(publishPacket("device/\(config.serial)/request",
                               #"{"pushing":{"sequence_id":"0","command":"pushall"}}"#))
            startPing()
        case 0x30:   // PUBLISH
            guard b.count >= 2 else { return }
            let tlen = Int(b[0]) << 8 | Int(b[1])
            var off = 2 + tlen
            if (type >> 1) & 0x03 > 0 { off += 2 }    // QoS > 0 → identyfikator pakietu
            guard off <= b.count else { return }
            let payload = Data(b[off...])
            ingest(payload)
            if let raw = onRawReport, let s = String(data: payload, encoding: .utf8) { raw(s) }
        default:     // SUBACK, PINGRESP — nic do zrobienia
            break
        }
    }

    // MARK: - Stan

    private func ingest(_ payload: Data) {
        guard let obj = try? JSONSerialization.jsonObject(with: payload) as? [String: Any],
              let p = obj["print"] as? [String: Any] else { return }
        merged = Self.merge(merged, p)
        let s = Self.extract(merged)
        lock.lock(); _state = s; lock.unlock()
    }

    /// Scala częściowy raport (słowniki rekurencyjnie, reszta nadpisywana).
    static func merge(_ a: [String: Any], _ b: [String: Any]) -> [String: Any] {
        var out = a
        for (k, v) in b {
            if let vd = v as? [String: Any], let ad = out[k] as? [String: Any] {
                out[k] = merge(ad, vd)
            } else {
                out[k] = v
            }
        }
        return out
    }

    /// Bambu podaje liczby raz jako liczby, raz jako tekst ("220.0").
    static func num(_ v: Any?) -> Double? {
        if let n = v as? NSNumber { return n.doubleValue }
        if let s = v as? String { return Double(s) }
        return nil
    }

    static func extract(_ p: [String: Any]) -> PrinterState {
        var s = PrinterState()
        s.state = p["gcode_state"] as? String
        s.percent = num(p["mc_percent"]).map { Int($0) }
        s.remainingMin = num(p["mc_remaining_time"]).map { Int($0) }
        s.layer = num(p["layer_num"]).map { Int($0) }
        s.totalLayers = num(p["total_layer_num"]).map { Int($0) }
        if let n = p["subtask_name"] as? String, !n.isEmpty {
            s.file = n
        } else if let g = p["gcode_file"] as? String, !g.isEmpty {
            s.file = (g as NSString).lastPathComponent
        }
        s.nozzle = num(p["nozzle_temper"])
        s.nozzleTarget = num(p["nozzle_target_temper"])
        s.bed = num(p["bed_temper"])
        s.bedTarget = num(p["bed_target_temper"])
        // P2S/H2D: komora w device.ctc.info.temp; starsze modele: chamber_temper
        s.chamber = num(p["chamber_temper"])
            ?? num(((p["device"] as? [String: Any])?["ctc"] as? [String: Any]).flatMap { $0["info"] as? [String: Any] }?["temp"])
        s.error = num(p["print_error"]).map { Int($0) }

        // Filament. Globalny indeks slotu = ams*4 + tray; 254 = szpula zewnętrzna, 255 = brak.
        // Źródła po kolei: ams.tray_now (ładowany teraz), device.extruder.info[0].snow
        // (P2S/H2D: ams<<8 | tray, 65535 = brak), mapping[0] (slot ostatniego zadania).
        let ams = p["ams"] as? [String: Any]
        let units = (ams?["ams"] as? [[String: Any]]) ?? []
        func trayAt(_ g: Int) -> [String: Any]? {
            if g == 254 || g == 255 {
                return (p["vt_tray"] as? [String: Any]) ?? (p["vir_slot"] as? [[String: Any]])?.first
            }
            let u = g / 4, t = g % 4
            guard let unit = units.first(where: { num($0["id"]).map(Int.init) == u }),
                  let trays = unit["tray"] as? [[String: Any]] else { return nil }
            return trays.first(where: { num($0["id"]).map(Int.init) == t })
        }
        var slot: Int?
        if let n = num(ams?["tray_now"]).map(Int.init), n != 255 { slot = n }
        if slot == nil,
           let ex = ((p["device"] as? [String: Any])?["extruder"] as? [String: Any])?["info"] as? [[String: Any]],
           let sn = num(ex.first?["snow"]).map(Int.init), sn != 65535 {
            let u = sn >> 8, t = sn & 0xFF
            slot = u == 255 ? 254 : u * 4 + t
        }
        if slot == nil, let m = (p["mapping"] as? [Any])?.first, let n = num(m).map(Int.init), n < 254 { slot = n }
        if let g = slot, let tray = trayAt(g) {
            s.filamentType = (tray["tray_type"] as? String).flatMap { $0.isEmpty ? nil : $0 }
            if let c = tray["tray_color"] as? String, c.count >= 6, c.prefix(6) != "000000" || s.filamentType != nil {
                s.filamentColor = String(c.prefix(6))
            }
            if let r = num(tray["remain"]).map(Int.init), r >= 0 { s.filamentRemain = r }
        }
        if let first = units.first, let trays = first["tray"] as? [[String: Any]] {
            s.amsColors = (0..<4).map { t in
                guard let tr = trays.first(where: { num($0["id"]).map(Int.init) == t }),
                      let c = tr["tray_color"] as? String, c.count >= 6,
                      (tr["tray_type"] as? String).map({ !$0.isEmpty }) ?? false else { return "" }
                return String(c.prefix(6))
            }
            if let g = slot, g < 4 { s.activeSlot = g }
        }
        s.updated = Date()
        return s
    }

    // MARK: - Tryb diagnostyczny

    /// `gpu-tray --bambu-dump <ip> <serial> <kod>` — wypisuje surowe raporty i wyciągnięty stan.
    static func runDump(args: [String]) {
        // bez argumentów: dane z ustawień aplikacji (defaults com.gputray.app bambuHost/…)
        guard let cfg = args.count >= 3 ? Config(host: args[0], serial: args[1], accessCode: args[2])
                                        : Config.fromDefaults() else {
            print("Użycie: gpu-tray --bambu-dump <ip drukarki> <numer seryjny> <kod dostępu LAN>")
            print("   albo ustaw bambuHost / bambuSerial / bambuAccessCode i uruchom bez argumentów")
            exit(1)
        }
        let c = BambuClient(config: cfg)
        c.onLink = { print("● łącze: \($0)") }
        var n = 0
        c.onRawReport = { raw in
            n += 1
            print("\n──── raport #\(n) (\(raw.utf8.count) B) ────")
            print(raw)
            let s = c.state
            print("→ stan: \(s.state ?? "-")  \(s.percent.map { "\($0)%" } ?? "-")  pozostało \(s.remainingMin.map { "\($0) min" } ?? "-")"
                + "  warstwa \(s.layer.map(String.init) ?? "-")/\(s.totalLayers.map(String.init) ?? "-")"
                + "  plik \(s.file ?? "-")")
            print("→ dysza \(s.nozzle.map { String($0) } ?? "-")/\(s.nozzleTarget.map { String($0) } ?? "-")"
                + "  stół \(s.bed.map { String($0) } ?? "-")/\(s.bedTarget.map { String($0) } ?? "-")"
                + "  komora \(s.chamber.map { String($0) } ?? "-")"
                + "  filament \(s.filamentType ?? "-") #\(s.filamentColor ?? "-") \(s.filamentRemain.map { "\($0)%" } ?? "")"
                + "  AMS \(s.amsColors) aktywny \(s.activeSlot.map(String.init) ?? "-")  błąd \(s.error.map(String.init) ?? "-")")
        }
        print("Łączę z \(cfg.host):8883 (Ctrl-C kończy)…")
        c.start()
        dispatchMain()
    }
}
