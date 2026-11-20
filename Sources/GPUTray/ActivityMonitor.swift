import Foundation

/// Wykrywa, czy Ollama właśnie generuje (stan: bezczynny ↔ pracuje).
/// Sygnał: użycie GPU ≥ próg przy załadowanym modelu, podtrzymane przez 2 s;
/// koniec po 3 s ciszy. Debounce eliminuje mruganie przy krótkich zapytaniach.
@MainActor
final class ActivityMonitor {
    struct Transition: Sendable {
        let started: Bool
        let ended: Bool
        let duration: TimeInterval?
    }

    private(set) var isBusy = false
    private(set) var busySince: Date?

    private var highSamples = 0
    private var lowSamples = 0

    private var threshold: Double {
        UserDefaults.standard.object(forKey: "gpuBusyThreshold") == nil
            ? 60.0
            : UserDefaults.standard.double(forKey: "gpuBusyThreshold")
    }

    /// Wywoływane co 1 s.
    func update(gpu: Double?, modelsLoaded: Bool) -> Transition {
        let active = (gpu ?? 0) >= threshold && modelsLoaded
        if active {
            lowSamples = 0
            highSamples += 1
            if !isBusy && highSamples >= 2 {
                isBusy = true
                busySince = Date()
                return Transition(started: true, ended: false, duration: nil)
            }
        } else {
            highSamples = 0
            if isBusy {
                lowSamples += 1
                if lowSamples >= 3 {
                    let dur = Date().timeIntervalSince(busySince ?? Date())
                    isBusy = false
                    busySince = nil
                    return Transition(started: false, ended: true, duration: dur)
                }
            } else {
                lowSamples = 0
            }
        }
        return Transition(started: false, ended: false, duration: nil)
    }
}
