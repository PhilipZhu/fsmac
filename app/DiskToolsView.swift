// FSMac app: Check Disk and Erase windows.

import AppKit
import SwiftUI

/// Scrolling, selectable tool output.
struct OutputLog: View {
    var lines: [String]

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                Text(lines.joined(separator: "\n"))
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
                Color.clear.frame(height: 1).id("end")
            }
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color.secondary.opacity(0.3)))
            .onChange(of: lines.count) { _, _ in proxy.scrollTo("end", anchor: .bottom) }
        }
    }
}

struct CheckView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    let diskID: String
    @State private var lines: [String] = []
    @State private var running = false
    @State private var result: (ok: Bool, message: String)?

    private var volume: VolumeInfo? { model.volume(diskID) }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Image(systemName: "stethoscope")
                    .font(.system(size: 28))
                    .foregroundStyle(.tint)
                VStack(alignment: .leading) {
                    Text("Check “\(volume?.displayName ?? diskID)”").font(.headline)
                    Text(subtitle).font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            OutputLog(lines: lines)
                .frame(minHeight: 200)
            if let result {
                Label(result.message, systemImage: result.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                    .foregroundStyle(result.ok ? .green : .red)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                if running { ProgressView().controlSize(.small) }
                Spacer()
                Button("Close") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                    .disabled(running)
                Button("Check") { run(repair: false) }
                    .disabled(running || volume == nil || !model.isAdmin)
                Button("Repair") { run(repair: true) }
                    .keyboardShortcut(.defaultAction)
                    .disabled(running || volume == nil || !model.isAdmin)
            }
            if !model.isAdmin {
                Text("Checking disks requires an administrator account.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(18)
        .frame(minWidth: 520, minHeight: 380)
    }

    private var subtitle: String {
        guard let v = volume else { return "The disk is no longer connected." }
        if v.fsType == "ntfs" {
            return "Checks the NTFS structures and resets the journal. Windows' chkdsk can repair more problems."
        }
        return "Runs e2fsck. A mounted volume is unmounted during the check and mounted again afterwards."
    }

    private func run(repair: Bool) {
        lines = []
        result = nil
        running = true
        model.check(diskID, repair: repair, output: { line in
            lines.append(line)
        }, done: { ok, message in
            running = false
            result = (ok, message)
        })
    }
}

struct FormatView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    let diskID: String
    @State private var type = "ntfs"
    @State private var label = "Untitled"
    @State private var wholeDisk = false
    @State private var lines: [String] = []
    @State private var running = false
    @State private var result: (ok: Bool, message: String)?
    @State private var labelLoaded = false

    private var volume: VolumeInfo? { model.volume(diskID) }
    private var target: String { wholeDisk ? (volume?.wholeDisk ?? diskID) : diskID }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 10) {
                Image(systemName: "externaldrive.badge.xmark")
                    .font(.system(size: 28))
                    .foregroundStyle(.red)
                VStack(alignment: .leading) {
                    Text("Erase “\(volume?.displayName ?? diskID)”?").font(.headline)
                    Text("Erasing deletes all data on it permanently. This cannot be undone.")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }
            Form {
                TextField("Name:", text: $label)
                Picker("Format:", selection: $type) {
                    Text("NTFS (Windows)").tag("ntfs")
                    Text("ext4 (Linux)").tag("ext4")
                }
                if let v = volume, v.wholeDisk != v.id {
                    Picker("Erase:", selection: $wholeDisk) {
                        Text("This volume only (\(v.id))").tag(false)
                        Text("The entire disk (\(v.wholeDisk))").tag(true)
                    }
                    .pickerStyle(.radioGroup)
                }
            }
            .disabled(running)
            if !lines.isEmpty {
                OutputLog(lines: lines).frame(minHeight: 140)
            }
            if let result {
                Label(result.message, systemImage: result.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                    .foregroundStyle(result.ok ? .green : .red)
            }
            HStack {
                if running { ProgressView().controlSize(.small) }
                Spacer()
                Button(result?.ok == true ? "Done" : "Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                    .disabled(running)
                if result?.ok != true {
                    Button("Erase") { confirm() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(running || volume == nil || label.isEmpty || !model.isAdmin)
                }
            }
            if !model.isAdmin {
                Text("Erasing disks requires an administrator account.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(18)
        .frame(minWidth: 480)
        .onAppear {
            guard !labelLoaded, let v = volume else { return }
            labelLoaded = true
            label = v.label.isEmpty ? "Untitled" : v.label
            type = v.fsType == "ntfs" ? "ntfs" : "ext4"
        }
    }

    private func confirm() {
        guard let v = volume else { return }
        let alert = NSAlert()
        alert.messageText = wholeDisk
            ? "Erase the entire disk “\(v.deviceName.isEmpty ? v.wholeDisk : v.deviceName)”?"
            : "Erase “\(v.displayName)”?"
        alert.informativeText = wholeDisk
            ? "All volumes on \(v.wholeDisk) will be deleted and replaced by a single \(type == "ntfs" ? "NTFS" : "ext4") volume."
            : "All data on \(v.id) will be deleted."
        alert.alertStyle = .critical
        alert.addButton(withTitle: "Erase")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        lines = []
        result = nil
        running = true
        model.format(target, type: type, label: label, force: false, output: { lines.append($0) },
                     done: { ok, message in
                         running = false
                         result = (ok, message)
                     })
    }
}
