import AppKit

/// Kontroler ikony w pasku menu + menu rozwijanego.
@MainActor
final class StatusBarController: NSObject, NSMenuDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu(title: "GPU Tray")

    private let ollama = OllamaClient()
    private let logParser = OllamaLogParser()
    private let activity = ActivityMonitor()
    private let net = NetMonitor()
    private let cpu = CpuMonitor()
    private let espLink = ESP32Link()

    private var gpuTimer: Timer?
    private var ollamaTimer: Timer?
    private var pulseTimer: Timer?

    // Aktualny stan (main actor)
    private var gpu: GPUSample?
    private var gpuEMA: Double = 0
    private var models: [RunningModel] = []
    private var reachable = false
    private var hostMem: (used: Int64, total: Int64)?
    private var netRate: (down: Double, up: Double)?   // MB/s
    private var cpuPct: Double?
    private var lastUpdate = Date.distantPast

    override init() {
        super.init()
        menu.delegate = self
        statusItem.menu = menu
        updateTitle()
        logParser.start()
        espLink.start()

        gpuTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tickSecond() }
        }
        ollamaTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tickOllama() }
        }
        tickOllama()
    }

    // MARK: - Cykle odświeżania

    /// Co 1 s: odczyt GPU, pamięci, sieci, CPU, maszyna stanów Ollama, tytuł ikony + ekran ESP32.
    private func tickSecond() {
        gpu = GPUReader.read()
        if let g = gpu {
            gpuEMA = gpuEMA <= 0 ? g.deviceUtil : gpuEMA * 0.6 + g.deviceUtil * 0.4
        }
        hostMem = HostMemory.read()
        netRate = net.update()
        cpuPct = cpu.update()

        let t = activity.update(gpu: gpu?.deviceUtil, modelsLoaded: !models.isEmpty)
        if t.started {
            DebugLog.log("BUSY start — GPU \(Formatters.pct(gpuEMA))")
            Notifier.notify(title: "⚡ Ollama zaczął pracę", body: busyStartBody)
        }
        if t.ended, let dur = t.duration {
            DebugLog.log("BUSY koniec — trwało \(Formatters.duration(dur))")
            Notifier.notify(title: "✅ Ollama zakończył", body: busyEndBody(duration: dur))
        }

        lastUpdate = Date()
        espLink.publish(buildLinkSnapshot())
        updateTitle()
    }

    /// Składa pakiet danych dla ekranu ESP32 (GPU, RAM, sieć, CPU, status Ollama).
    private func buildLinkSnapshot() -> ESP32Link.Snapshot {
        var s = ESP32Link.Snapshot()
        s.time = Formatters.time.string(from: Date())
        s.gpu = gpu?.deviceUtil
        s.cpu = cpuPct
        let gb = 1_073_741_824.0
        if let hm = hostMem {
            s.memUsedGB = Double(hm.used) / gb
            s.memTotalGB = Double(hm.total) / gb
        }
        if let g = gpu { s.vgpuGB = Double(g.inUseMemory) / gb }
        if let r = netRate {
            s.netDownMB = r.down
            s.netUpMB = r.up
        }
        s.busy = activity.isBusy
        if activity.isBusy {
            s.model = models.first?.shortName ?? "?"
            if let since = activity.busySince {
                s.genSeconds = Int(Date().timeIntervalSince(since))
            }
        }
        // tok/s tylko z ostatniej ukończonej generacji (log wypisuje czas po zakończeniu)
        if let stat = logParser.lastStat, Date().timeIntervalSince(stat.date) < 900 {
            s.tokPerSec = stat.tokPerSec
            s.tokAge = Int(Date().timeIntervalSince(stat.date))
        }
        return s
    }

    /// Co 2 s: odpytanie serwera Ollama o załadowane modele.
    private func tickOllama() {
        Task { @MainActor in
            await ollama.refreshBaseURL()
            let fetched = await ollama.fetchRunningModels()
            reachable = fetched != nil
            if let fetched { models = fetched }
        }
    }

    // MARK: - Ikona w pasku menu

    private func updateTitle() {
        guard let button = statusItem.button else { return }
        setPulseTimer(activity.isBusy)

        let font = NSFont.monospacedDigitSystemFont(ofSize: 13, weight: .regular)
        let text = NSMutableAttributedString()

        if gpu == nil {
            text.append(NSAttributedString(string: "—", attributes: [
                .font: font, .foregroundColor: NSColor.secondaryLabelColor,
            ]))
        } else {
            var color: NSColor = .labelColor
            if activity.isBusy {
                // Pulsowanie: pomarańczowy ↔ czerwony, ~1.4 Hz
                let t = Date().timeIntervalSinceReferenceDate
                let phase = (sin(t * 2 * Double.pi * 1.4) + 1) / 2
                let dot = NSColor.systemOrange
                    .blended(withFraction: CGFloat(0.3 + 0.5 * phase), of: NSColor.systemRed) ?? .systemOrange
                text.append(NSAttributedString(string: "●", attributes: [
                    .font: NSFont.systemFont(ofSize: 9),
                    .foregroundColor: dot.withAlphaComponent(CGFloat(0.35 + 0.65 * phase)),
                    .baselineOffset: 3.0,
                ]))
                color = dot
            } else if gpuEMA >= 85 {
                color = .systemRed
            } else if gpuEMA >= 50 {
                color = .systemOrange
            }
            text.append(NSAttributedString(string: " \(Int(gpuEMA.rounded()))%", attributes: [
                .font: font, .foregroundColor: color,
            ]))
        }
        button.attributedTitle = text
    }

    private func setPulseTimer(_ on: Bool) {
        if on, pulseTimer == nil {
            pulseTimer = Timer.scheduledTimer(withTimeInterval: 0.12, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.updateTitle() }
            }
        } else if !on, let t = pulseTimer {
            t.invalidate()
            pulseTimer = nil
        }
    }

    // MARK: - Treść powiadomień

    private var busyStartBody: String {
        let name = models.first?.shortName ?? "model"
        return "Model: \(name) — GPU \(Formatters.pct(gpuEMA))"
    }

    private func busyEndBody(duration: TimeInterval) -> String {
        var s = "Czas: \(Formatters.duration(duration))"
        if let stat = logParser.lastStat, Date().timeIntervalSince(stat.date) < 20 {
            s += String(format: " · %.1f tok/s", stat.tokPerSec)
        }
        return s
    }

    // MARK: - Menu rozwijane

    func menuNeedsUpdate(_ menu: NSMenu) {
        rebuildMenu()
    }

    private func rebuildMenu() {
        menu.removeAllItems()

        // Nagłówek statusu
        let header = NSMenuItem()
        header.isEnabled = false
        let hTitle: String
        if activity.isBusy {
            let name = models.first?.shortName ?? "?"
            hTitle = "⚡ Ollama pracuje — \(name)"
        } else if !reachable {
            hTitle = ollama.baseURL == nil
                ? "⚠️ Nie znaleziono serwera Ollama"
                : "⚠️ Ollama nie odpowiada"
        } else if models.isEmpty {
            hTitle = "💤 Ollama bezczynny — brak modelu w pamięci"
        } else {
            hTitle = "💤 Ollama bezczynny"
        }
        header.attributedTitle = NSAttributedString(string: hTitle, attributes: [
            .font: NSFont.boldSystemFont(ofSize: 13),
        ])
        menu.addItem(header)

        if activity.isBusy, let since = activity.busySince {
            addInfo("    ⏱ Generowanie trwa: \(Formatters.duration(Date().timeIntervalSince(since)))")
        }
        menu.addItem(.separator())

        // Sekcja GPU
        if let g = gpu {
            addBar("GPU (ogółem)", g.deviceUtil, Formatters.pct(g.deviceUtil), bold: true)
            addBar("Renderer", g.rendererUtil, Formatters.pct(g.rendererUtil))
            addBar("Tiler", g.tilerUtil, Formatters.pct(g.tilerUtil))
            if let total = hostMem?.total, total > 0 {
                addBar("Pamięć GPU", Double(g.inUseMemory) / Double(total),
                       "\(Formatters.bytes(g.inUseMemory)) / \(Formatters.bytes(total))")
            }
        } else {
            addInfo("Brak odczytu GPU (klasa AGXAccelerator)")
        }
        if let hm = hostMem {
            addBar("Pamięć zintegrowana", Double(hm.used) / Double(hm.total),
                   "\(Formatters.bytes(hm.used)) / \(Formatters.bytes(hm.total))")
        }
        menu.addItem(.separator())

        // Sekcja modeli Ollama
        if reachable {
            if models.isEmpty {
                addInfo("Brak załadowanych modeli — pojawią się po pierwszym zapytaniu")
            } else {
                for (i, m) in models.enumerated() {
                    let params = (m.details?.parameterSize).map { " (\($0))" } ?? ""
                    addInfo("🧠 \(m.shortName)\(params)", bold: i == 0)
                    addInfo("    W VRAM: \(Formatters.bytes(m.sizeVram)) — \(m.gpuPercent)% GPU")
                    if let ctx = m.contextLength {
                        addInfo("    Kontekst: \(Formatters.int(ctx))")
                    }
                    addInfo("    Auto-unload: \(Formatters.countdown(to: m.expiresAt) ?? "bez limitu")")
                }
                if let stat = logParser.lastStat {
                    addInfo("⏩ Ostatnia generacja: \(String(format: "%.1f", stat.tokPerSec)) tok/s (\(Formatters.timeShort.string(from: stat.date)))")
                } else {
                    addInfo("⏩ Brak danych o generowaniu (czekam na wpisy w logu)")
                }
            }
        }
        menu.addItem(.separator())

        // Akcje
        if models.count == 1, let m = models.first {
            let item = NSMenuItem(title: "⏹ Zatrzymaj model (zwolnij VRAM)", action: #selector(unloadModelAction(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = m.name
            menu.addItem(item)
        } else if models.count > 1 {
            let sub = NSMenuItem(title: "⏹ Zatrzymaj model", action: nil, keyEquivalent: "")
            let subMenu = NSMenu()
            for m in models {
                let i = NSMenuItem(title: m.shortName, action: #selector(unloadModelAction(_:)), keyEquivalent: "")
                i.target = self
                i.representedObject = m.name
                subMenu.addItem(i)
            }
            sub.submenu = subMenu
            menu.addItem(sub)
        }
        if isAppBundle {
            let auto = NSMenuItem(title: "🚀 Uruchamiaj przy logowaniu", action: #selector(toggleAutostartAction), keyEquivalent: "")
            auto.target = self
            auto.state = Autostart.isEnabled ? .on : .off
            menu.addItem(auto)
        }
        let logs = NSMenuItem(title: "📄 Pokaż logi Ollama", action: #selector(showLogsAction), keyEquivalent: "")
        logs.target = self
        menu.addItem(logs)
        addInfo("🖥 Ekran ESP32: \(espLink.peerLabel() ?? "nie pyta — wyłączony?")")

        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Zakończ GPU Tray", action: #selector(quitAction), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)

        let footer = NSMenuItem()
        footer.isEnabled = false
        footer.attributedTitle = NSAttributedString(
            string: "Odświeżono: \(Formatters.time.string(from: lastUpdate))",
            attributes: [.font: NSFont.systemFont(ofSize: 10), .foregroundColor: NSColor.secondaryLabelColor]
        )
        menu.addItem(footer)
    }

    // MARK: - Elementy pomocnicze menu

    private func addInfo(_ text: String, bold: Bool = false) {
        let item = NSMenuItem()
        item.isEnabled = false
        let font = bold ? NSFont.boldSystemFont(ofSize: 12) : NSFont.menuFont(ofSize: 0)
        item.attributedTitle = NSAttributedString(string: text, attributes: [.font: font])
        menu.addItem(item)
    }

    private func addBar(_ label: String, _ fraction: Double, _ rightText: String, bold: Bool = false) {
        let view = BarView(label: label, fraction: fraction, rightText: rightText, bold: bold)
        let item = NSMenuItem(title: label, action: nil, keyEquivalent: "")
        item.view = view
        menu.addItem(item)
    }

    private var isAppBundle: Bool {
        Bundle.main.infoDictionary?["CFBundleIdentifier"] as? String != nil
    }

    // MARK: - Akcje

    @objc private func unloadModelAction(_ sender: NSMenuItem) {
        guard let name = sender.representedObject as? String else { return }
        DebugLog.log("Akcja: unload \(name)")
        Task { @MainActor in
            _ = await ollama.unloadModel(name)
            tickOllama()
        }
    }

    @objc private func toggleAutostartAction() {
        Autostart.setEnabled(!Autostart.isEnabled)
    }

    @objc private func showLogsAction() {
        let dir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".ollama/logs")
        if FileManager.default.fileExists(atPath: dir.path) {
            NSWorkspace.shared.open(dir)
        }
    }

    @objc private func quitAction() {
        NSApp.terminate(nil)
    }
}

/// Wiersz menu z etykietą, paskiem postępu i wartością po prawej.
private final class BarView: NSView {
    private let label: String
    private let fraction: Double
    private let rightText: String
    private let bold: Bool

    init(label: String, fraction: Double, rightText: String, bold: Bool) {
        self.label = label
        self.fraction = min(max(fraction, 0), 1)
        self.rightText = rightText
        self.bold = bold
        super.init(frame: NSRect(x: 0, y: 0, width: 300, height: 24))
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) nie jest wspierane") }

    override func draw(_ dirtyRect: NSRect) {
        let labelFont = bold ? NSFont.boldSystemFont(ofSize: 12) : NSFont.menuFont(ofSize: 0)
        (label as NSString).draw(at: NSPoint(x: 16, y: 5), withAttributes: [
            .font: labelFont, .foregroundColor: NSColor.labelColor,
        ])

        let bar = NSRect(x: 150, y: 9, width: 90, height: 7)
        let track = NSBezierPath(roundedRect: bar, xRadius: 3.5, yRadius: 3.5)
        NSColor.quaternaryLabelColor.setFill()
        track.fill()
        if fraction > 0.005 {
            let fillRect = NSRect(x: bar.minX, y: bar.minY, width: bar.width * CGFloat(fraction), height: bar.height)
            let fill = NSBezierPath(roundedRect: fillRect, xRadius: 3.5, yRadius: 3.5)
            barColor.setFill()
            fill.fill()
        }

        let rightFont = NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        let attrs: [NSAttributedString.Key: Any] = [.font: rightFont, .foregroundColor: NSColor.labelColor]
        let size = (rightText as NSString).size(withAttributes: attrs)
        (rightText as NSString).draw(at: NSPoint(x: 292 - size.width, y: 6), withAttributes: attrs)
    }

    private var barColor: NSColor {
        if fraction >= 0.85 { return .systemRed }
        if fraction >= 0.5 { return .systemOrange }
        return .systemGreen
    }
}
