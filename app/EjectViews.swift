// FSMac app: ejecting disks, and what the menu shows afterwards.
//
// Ejecting is a disk-level action, like in Finder. While it runs the disk's
// header shows the current step; afterwards the disk is either shown as
// ejected ("Safe to unplug", until it is physically disconnected) or the
// reason it is still in use is shown, with a clear "not safe to unplug".

import AppKit
import SwiftUI

/// The header above a disk's volumes, with its Eject button.
struct DiskHeader: View {
    @EnvironmentObject var model: AppModel
    var disk: String
    var title: String
    var size: UInt64?
    var canEject: Bool

    private var progress: String? { model.ejectProgress[disk] }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Text(title.uppercased())
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                if let size {
                    Text(size.formattedBytes)
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
                Spacer(minLength: 6)
                if progress != nil {
                    ProgressView().controlSize(.small)
                } else if canEject {
                    Button {
                        model.ejectDisk(disk)
                    } label: {
                        Label("Eject", systemImage: "eject.fill")
                            .font(.caption.weight(.medium))
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .help("Eject disk")
                }
            }
            if let progress {
                Text(progress)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.horizontal, 14)
        .padding(.top, 10)
        .padding(.bottom, 2)
    }
}

/// Shown under a disk whose eject failed: it is still in use.
struct EjectErrorBox: View {
    @EnvironmentObject var model: AppModel
    var disk: String
    var message: String

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.octagon.fill")
                .foregroundStyle(.red)
            VStack(alignment: .leading, spacing: 5) {
                Text("Not ejected. Not safe to unplug.")
                    .font(.callout.weight(.semibold))
                Text(message)
                    .font(.caption)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 6) {
                    Button("Try Again") { model.ejectDisk(disk) }
                    Button("Eject Anyway…") { confirmForce() }
                    Spacer()
                    Button {
                        model.dismissEjectError(disk)
                    } label: {
                        Image(systemName: "xmark")
                    }
                    .buttonStyle(.borderless)
                    .help("Dismiss")
                }
                .controlSize(.small)
            }
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 8).fill(Color.red.opacity(0.08)))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.red.opacity(0.35)))
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
    }

    private func confirmForce() {
        let alert = NSAlert()
        alert.messageText = "Eject the disk while it is in use?"
        alert.informativeText = "Apps using it may lose unsaved changes."
        alert.alertStyle = .warning
        alert.addButton(withTitle: "Eject Anyway")
        alert.addButton(withTitle: "Cancel")
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertFirstButtonReturn {
            model.ejectDisk(disk, force: true)
        }
    }
}

/// A disk that has been ejected and is waiting to be unplugged.
struct EjectedDiskCard: View {
    @EnvironmentObject var model: AppModel
    var disk: EjectedDisk

    private var tint: Color { disk.verified ? .green : .orange }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: disk.verified ? "externaldrive.badge.checkmark" : "externaldrive.badge.exclamationmark")
                .font(.system(size: 22))
                .foregroundStyle(tint)
                .frame(width: 30)
            VStack(alignment: .leading, spacing: 3) {
                Text(disk.name)
                    .font(.system(.body, weight: .medium))
                    .lineLimit(1)
                Text(disk.verified ? "Safe to unplug" : "Ejected, not verified")
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(tint)
                Text(details)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let warning = disk.warning {
                    Text(warning)
                        .font(.caption)
                        .foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if disk.mediaPresent {
                    Button {
                        model.mountDisk(disk.id)
                    } label: {
                        Label("Mount Again", systemImage: "arrow.uturn.backward")
                    }
                    .controlSize(.small)
                    .disabled(model.pending.contains(disk.id))
                    .padding(.top, 2)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 8).fill(tint.opacity(0.08)))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(tint.opacity(0.35)))
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
    }

    private var details: String {
        let when = DateFormatter.localizedString(from: disk.date, dateStyle: .none, timeStyle: .short)
        let what = disk.volumes.isEmpty ? "" : disk.volumes.joined(separator: ", ") + " · "
        return "\(what)\(when)" + (disk.mediaPresent
            ? " · available to mount again" : " · reconnect to use again")
    }
}
