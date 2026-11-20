import Foundation
import UserNotifications

/// Powiadomienia systemowe macOS (start/koniec generowania).
/// Działa tylko w kontekście pakietu .app (wymagany CFBundleIdentifier).
enum Notifier {
    static var isAvailable: Bool {
        Bundle.main.infoDictionary?["CFBundleIdentifier"] as? String != nil
    }

    static func requestAuthorizationIfNeeded() {
        guard isAvailable else {
            DebugLog.log("Powiadomienia: pominięte (brak bundla — tryb diagnostyczny)")
            return
        }
        let center = UNUserNotificationCenter.current()
        center.delegate = NotificationCenterDelegate.shared
        center.requestAuthorization(options: [.alert, .sound]) { granted, error in
            if let error {
                DebugLog.log("Powiadomienia: błąd — \(error.localizedDescription)")
            } else {
                DebugLog.log("Powiadomienia: \(granted ? "zezwolono" : "odmowa użytkownika")")
            }
        }
    }

    static func notify(title: String, body: String) {
        guard isAvailable else { return }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        let req = UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil)
        UNUserNotificationCenter.current().add(req) { error in
            if let error { DebugLog.log("Powiadomienie: błąd — \(error.localizedDescription)") }
        }
    }
}

final class NotificationCenterDelegate: NSObject, UNUserNotificationCenterDelegate {
    static let shared = NotificationCenterDelegate()

    /// Pokazuj banery także, gdy aplikacja jest "aktywna" (menu bar app).
    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification,
        withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void
    ) {
        completionHandler([.banner, .sound])
    }
}
