import Foundation

/// Tryb `gpu-tray --screen-test`: odpowiada na CYDREQ danymi syntetycznymi.
/// Do weryfikacji ekranu (Wi-Fi, dotyk, rysowanie) bez uruchamiania GUI.
enum ScreenTest {
    static func run() {
        print("GPU Tray — tryb testowy ekranu ESP32 (--screen-test)")
        print("Nasłuchuję CYDREQ na UDP \(ESP32Link.port). Ctrl-C aby przerwać.")
        print("")

        let link = ESP32Link()
        link.onRequest = { ip in print("← REQ od \(ip) → odpowiadam danymi testowymi") }
        link.start()

        var t = 0.0
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
        timer.schedule(deadline: .now() + 0.5, repeating: 1.0)
        timer.setEventHandler {
            t += 1
            var s = ESP32Link.Snapshot()
            s.host = "demo-mac"               // neutralna nazwa (zrzuty ekranu do README)
            s.time = Formatters.time.string(from: Date())
            let wave = sin(t / 9.0)
            s.gpu = max(0, 40 + 35 * wave)
            s.cpu = max(0, 12 + 8 * sin(t / 5.0))
            s.memUsedGB = 10 + 2 * sin(t / 30.0)
            s.memTotalGB = 32
            s.vgpuGB = max(0, 4 + 2.5 * wave)
            s.netDownMB = max(0, 8 + 7 * sin(t / 13.0))
            s.netUpMB = max(0, 0.8 + 0.6 * sin(t / 7.0))
            s.busy = sin(t / 45.0) > 0
            if s.busy {
                s.model = "llama3.2:3b"
                s.genSeconds = Int(t.truncatingRemainder(dividingBy: 45))
                s.tokPerSec = 14.2 + 3 * wave
                s.tokAge = 30
            }
            // syntetyczny druk (neutralne dane do zrzutów README)
            var pr = PrinterState()
            pr.state = "RUNNING"
            let pct = Int(t / 3) % 101
            pr.percent = pct
            pr.remainingMin = max(0, 96 - pct * 96 / 100)
            pr.totalLayers = 240
            pr.layer = pct * 240 / 100
            pr.file = "benchy.3mf"
            pr.nozzle = 219.6 + 0.6 * sin(t / 3); pr.nozzleTarget = 220
            pr.bed = 60.2; pr.bedTarget = 60
            pr.chamber = 31
            pr.filamentType = "PLA"; pr.filamentColor = "FF6A13"; pr.filamentRemain = 72
            pr.amsColors = ["FF6A13", "FFFFFF", "161616", ""]
            pr.activeSlot = 0
            pr.updated = Date()
            s.printer = pr
            s.printerConfigured = true
            s.printerLinked = true
            link.publish(s)
        }
        timer.resume()
        dispatchMain()
    }
}
