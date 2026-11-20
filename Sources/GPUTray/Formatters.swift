import Foundation

/// Formatowanie wartości (polskie etykiety).
enum Formatters {
    static let time: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    static let timeShort: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm"
        return f
    }()

    private static let grouped: NumberFormatter = {
        let f = NumberFormatter()
        f.numberStyle = .decimal
        f.groupingSeparator = " "
        return f
    }()

    /// Bajty → GB w konwencji binarnej (GiB) — spójnie z `ollama ps` i "Ten Mac"
    /// (system_profiler "32 GB" = 32 GiB; ollama pokazuje 21 GB dziesiętnie jako "20 GB").
    static func bytes(_ v: Int64) -> String {
        let gib = Double(v) / 1_073_741_824
        if gib >= 10 { return String(format: "%.0f GB", gib) }
        return String(format: "%.1f GB", gib)
    }

    static func pct(_ v: Double) -> String {
        "\(Int(v.rounded()))%"
    }

    static func int(_ v: Int) -> String {
        grouped.string(from: NSNumber(value: v)) ?? "\(v)"
    }

    static func duration(_ t: TimeInterval) -> String {
        let s = Int(t.rounded())
        if s < 0 { return "0 s" }
        if s < 60 { return "\(s) s" }
        if s < 3600 { return String(format: "%d min %02d s", s / 60, s % 60) }
        return String(format: "%d godz %02d min", s / 3600, (s % 3600) / 60)
    }

    /// "za 27 min" / nil gdy data w przeszłości lub brak.
    static func countdown(to date: Date?, now: Date = Date()) -> String? {
        guard let date else { return nil }
        let left = date.timeIntervalSince(now)
        guard left > 0 else { return nil }
        return "za " + duration(left)
    }
}
