import Foundation

/// Tryb diagnostyczny (`gpu-tray --once`): jeden snapshot wszystkich źródeł danych na stdout.
/// Uwaga: w "gołym" binarium bez Info.plist ATS może blokować http po LAN —
/// pełny auto-detek działa w aplikacji .app (NSAppTransportSecurity w Info.plist).
enum Diagnostics {
    static func runOnce() {
        print("GPU Tray — tryb diagnostyczny (--once)")
        print("")

        if let g = GPUReader.read() {
            print("GPU:   ogółem \(Formatters.pct(g.deviceUtil)) | renderer \(Formatters.pct(g.rendererUtil)) | tiler \(Formatters.pct(g.tilerUtil))")
            print("       pamięć GPU: w użyciu \(Formatters.bytes(g.inUseMemory)) | przydzielona \(Formatters.bytes(g.allocMemory))")
        } else {
            print("GPU:   brak danych (nie znaleziono AGXAccelerator)")
        }

        if let hm = HostMemory.read() {
            print("RAM:   \(Formatters.bytes(hm.used)) / \(Formatters.bytes(hm.total))")
        }

        // Sieć + CPU: dwa punkty pomiarowe w odstępie 1 s (dla delty)
        let net = NetMonitor()
        let cpu = CpuMonitor()
        _ = net.update()
        _ = cpu.update()
        print("NET/CPU: mierzę przez 1 s…")
        Thread.sleep(forTimeInterval: 1.0)
        if let r = net.update() {
            print("NET:   ↓ \(String(format: "%.2f", r.down)) MB/s | ↑ \(String(format: "%.2f", r.up)) MB/s (interfejsy en*)")
        } else {
            print("NET:   brak odczytu interfejsów")
        }
        if let c = cpu.update() {
            print("CPU:   \(Formatters.pct(c))")
        } else {
            print("CPU:   brak odczytu")
        }

        let client = OllamaClient()
        let sem = DispatchSemaphore(value: 0)
        Task.detached {
            await client.refreshBaseURL(force: true)
            if let base = client.baseURL {
                print("Ollama: serwer \(base.absoluteString)")
            } else {
                print("Ollama: nie znaleziono serwera (patrz uwaga o ATS wyżej)")
            }
            let models = await client.fetchRunningModels()
            if let models {
                print("       modele (\(models.count)):")
                for m in models {
                    print("       • \(m.name)")
                    print("         VRAM: \(Formatters.bytes(m.sizeVram)) — \(m.gpuPercent)% GPU, kontekst: \(m.contextLength.map(Formatters.int) ?? "?")")
                    print("         auto-unload: \(Formatters.countdown(to: m.expiresAt) ?? "bez limitu")")
                }
            } else {
                print("       /api/ps niedostępne")
            }
            sem.signal()
        }
        sem.wait()

        let parser = OllamaLogParser()
        if let s = parser.scanHistory(chunkBytes: 512 * 1024) {
            let prompt = s.promptTokPerSec.map { String(format: "%.1f", $0) } ?? "?"
            print("Log:   ostatnia generacja \(String(format: "%.1f", s.tokPerSec)) tok/s (\(s.evalTokens) tok, prefill: \(prompt) tok/s)")
        } else {
            print("Log:   brak wcześniejszych generacji w ~/.ollama/logs/")
        }
    }
}
