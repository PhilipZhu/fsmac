// FSMac app: installing the privileged helper, notifications, login item.

import AppKit
import ServiceManagement
import UserNotifications

/// The LaunchDaemon (fsmacd) that does the actual work as root.
enum HelperStatus: Equatable {
    case notInstalled
    case requiresApproval
    case enabled
    /// Installed with `fsmac install-helper` (a plain LaunchDaemon plist).
    case installedManually
    case unavailable(String)

    static let plistName = "\(FSMac.daemonLabel).plist"
    static var service: SMAppService { SMAppService.daemon(plistName: plistName) }
    static let manualPlist = "/Library/LaunchDaemons/\(FSMac.daemonLabel).plist"

    static func current() -> HelperStatus {
        if FileManager.default.fileExists(atPath: manualPlist) { return .installedManually }
        switch service.status {
        case .enabled: return .enabled
        case .requiresApproval: return .requiresApproval
        case .notRegistered: return .notInstalled
        case .notFound:
            // Also reported for a daemon that was never registered.
            let plist = Bundle.main.bundleURL.appendingPathComponent("Contents/Library/LaunchDaemons/\(plistName)")
            return FileManager.default.fileExists(atPath: plist.path)
                ? .notInstalled : .unavailable("The helper is missing from the app bundle.")
        @unknown default: return .notInstalled
        }
    }

    static func register() throws {
        // Re-registering refreshes launchd's copy after an app update.
        if service.status == .enabled { try? service.unregister() }
        try service.register()
    }

    static func unregister() {
        try? service.unregister()
        if FileManager.default.fileExists(atPath: manualPlist) {
            _ = runPrivileged("'\(cliPath)' uninstall-helper")
        }
    }

    static var cliPath: String {
        Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/fsmac").path
    }

    /// Fallback when Login Items registration is not possible: installs a
    /// LaunchDaemon plist after asking for an administrator password.
    static func installWithAdministratorPrivileges() -> String? {
        runPrivileged("'\(cliPath)' install-helper")
    }

    /// Installs the `fsmac` command in /usr/local/bin.
    static func installCommandLineTool() -> String? {
        runPrivileged("mkdir -p /usr/local/bin && ln -sf '\(cliPath)' /usr/local/bin/fsmac")
    }

    static var commandLineToolInstalled: Bool {
        (try? FileManager.default.destinationOfSymbolicLink(atPath: "/usr/local/bin/fsmac")) == cliPath
    }

    @discardableResult
    private static func runPrivileged(_ command: String) -> String? {
        let escaped = command.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        let source = "do shell script \"\(escaped)\" with administrator privileges"
        var error: NSDictionary?
        NSAppleScript(source: source)?.executeAndReturnError(&error)
        if let error {
            if (error[NSAppleScript.errorNumber] as? Int) == -128 { return nil } // cancelled
            return error[NSAppleScript.errorMessage] as? String ?? "The command failed."
        }
        return nil
    }
}

/// "Open at Login" for the app itself.
enum LoginItem {
    static var isEnabled: Bool { SMAppService.mainApp.status == .enabled }

    static func set(_ on: Bool) {
        do {
            if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
        } catch {
            NSLog("FSMac: login item: \(error.localizedDescription)")
        }
    }
}

enum Notifier {
    /// UserNotifications only works for a bundled app.
    private static var available: Bool { Bundle.main.bundleIdentifier != nil }

    static func requestAuthorization() {
        guard available else { return }
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    static func post(title: String, body: String, id: String?) {
        guard available else { return }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        let req = UNNotificationRequest(identifier: id ?? UUID().uuidString, content: content, trigger: nil)
        UNUserNotificationCenter.current().add(req)
    }
}
