// FSMac daemon: the physical device behind a disk, flushing its cache and
// keeping it awake.
//
// After an eject, many USB drives stay attached until they are unplugged,
// and the ones that do go away leave the USB device behind until the cable
// is pulled. Watching that device lets FSMac show "safe to unplug" for
// exactly as long as the drive is still connected.

import Foundation
import IOKit
import IOUSBHost

enum PhysicalDevice {
    /// Registry entry ID of the USB device (or Thunderbolt-tunnelled PCI
    /// device) that holds a disk, or nil (internal, virtual, unknown).
    static func id(forBSDName bsd: String) -> UInt64? {
        guard let match = IOBSDNameMatching(kIOMainPortDefault, 0, bsd) else { return nil }
        var entry = IOServiceGetMatchingService(kIOMainPortDefault, match)
        while entry != 0 {
            if IOObjectConformsTo(entry, "IOUSBHostDevice") != 0 || IOObjectConformsTo(entry, "IOUSBDevice") != 0 ||
                isTunnelledPCI(entry) {
                var id: UInt64 = 0
                let kr = IORegistryEntryGetRegistryEntryID(entry, &id)
                IOObjectRelease(entry)
                return kr == KERN_SUCCESS ? id : nil
            }
            var parent: io_registry_entry_t = 0
            let kr = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent)
            IOObjectRelease(entry)
            entry = kr == KERN_SUCCESS ? parent : 0
        }
        return nil
    }

    private static func isTunnelledPCI(_ entry: io_registry_entry_t) -> Bool {
        guard IOObjectConformsTo(entry, "IOPCIDevice") != 0,
              let v = IORegistryEntryCreateCFProperty(entry, "IOPCITunnelled" as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() else { return false }
        return (v as? Bool) == true
    }

    /// Whether the device is still connected.
    static func isPresent(_ id: UInt64) -> Bool {
        let s = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(id))
        guard s != 0 else { return false }
        IOObjectRelease(s)
        return true
    }

    /// The device's name as it reports it (e.g. "ST12000NM005J").
    static func name(_ id: UInt64) -> String? {
        let s = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(id))
        guard s != 0 else { return nil }
        defer { IOObjectRelease(s) }
        var buf = [CChar](repeating: 0, count: 128)
        guard IORegistryEntryGetName(s, &buf) == KERN_SUCCESS else { return nil }
        let n = String(cString: buf).trimmingCharacters(in: .whitespaces)
        return n.isEmpty ? nil : n
    }

    /// A name for the drive that stays the same when it is connected again,
    /// e.g. "ST12000NM005J #ZRT14Y4Q [1111:2222]". USB devices only.
    static func identity(_ id: UInt64) -> String? {
        let s = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(id))
        guard s != 0 else { return nil }
        defer { IOObjectRelease(s) }
        guard IOObjectConformsTo(s, "IOUSBHostDevice") != 0 else { return nil }
        func prop(_ key: String) -> Any? {
            IORegistryEntryCreateCFProperty(s, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
        }
        func text(_ keys: String...) -> String {
            keys.lazy.compactMap { prop($0) as? String }.first?.trimmingCharacters(in: .whitespaces) ?? ""
        }
        guard let vid = prop("idVendor") as? Int, let pid = prop("idProduct") as? Int else { return nil }
        let product = text("USB Product Name", "kUSBProductString")
        let serial = text("USB Serial Number", "kUSBSerialNumberString")
        var parts = [product.isEmpty ? "USB disk" : product]
        if !serial.isEmpty { parts.append("#" + serial) }
        parts.append(String(format: "[%04x:%04x]", vid, pid))
        return parts.joined(separator: " ")
    }

    /// How long (seconds) a disk may be idle before macOS lowers its power
    /// state by one step, or nil if macOS doesn't manage its power.
    static func idleTimerPeriod(forBSDName bsd: String) -> TimeInterval? {
        guard let match = IOBSDNameMatching(kIOMainPortDefault, 0, bsd) else { return nil }
        var entry = IOServiceGetMatchingService(kIOMainPortDefault, match)
        while entry != 0 {
            let pm = IORegistryEntryCreateCFProperty(entry, "IOPowerManagement" as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? [String: Any]
            if let ms = pm?["IdleTimerPeriod"] as? Int, ms > 0 {
                IOObjectRelease(entry)
                return TimeInterval(ms) / 1000
            }
            var parent: io_registry_entry_t = 0
            let kr = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent)
            IOObjectRelease(entry)
            entry = kr == KERN_SUCCESS ? parent : 0
        }
        return nil
    }

    /// USB storage devices that are still attached but whose disk is gone:
    /// the SCSI logical unit has nothing above it any more. This is what a
    /// USB bridge leaves behind when its drive stops answering. (An empty
    /// card reader keeps its storage driver, so it does not count.)
    static func stalledStorageDevices() -> Set<UInt64> {
        var result: Set<UInt64> = []
        for cls in ["IOSCSIHierarchicalLogicalUnit", "IOSCSILogicalUnitNub"] {
            var iter: io_iterator_t = 0
            guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(cls), &iter) == KERN_SUCCESS
            else { continue }
            defer { IOObjectRelease(iter) }
            while case let lun = IOIteratorNext(iter), lun != 0 {
                defer { IOObjectRelease(lun) }
                var children: io_iterator_t = 0
                guard IORegistryEntryGetChildIterator(lun, kIOServicePlane, &children) == KERN_SUCCESS else { continue }
                let child = IOIteratorNext(children)
                IOObjectRelease(children)
                if child != 0 {
                    IOObjectRelease(child)
                    continue
                }
                if let id = usbAncestor(of: lun) { result.insert(id) }
            }
        }
        return result
    }

    private static func usbAncestor(of entry: io_registry_entry_t) -> UInt64? {
        var cur = entry
        IOObjectRetain(cur)
        while cur != 0 {
            if IOObjectConformsTo(cur, "IOUSBHostDevice") != 0 {
                var id: UInt64 = 0
                let kr = IORegistryEntryGetRegistryEntryID(cur, &id)
                IOObjectRelease(cur)
                return kr == KERN_SUCCESS ? id : nil
            }
            var parent: io_registry_entry_t = 0
            let kr = IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent)
            IOObjectRelease(cur)
            cur = kr == KERN_SUCCESS ? parent : 0
        }
        return nil
    }

    /// Resets a USB device and lets macOS attach its drivers again, as if it
    /// had been unplugged and connected again. Root only. Returns an error
    /// message, or nil.
    static func reconnect(_ id: UInt64) -> String? {
        let s = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(id))
        guard s != 0 else { return "the device is no longer connected" }
        defer { IOObjectRelease(s) }
        guard IOObjectConformsTo(s, "IOUSBHostDevice") != 0 else { return "not a USB device" }
        do {
            // Capturing detaches every driver; releasing the capture resets
            // the device and lets drivers match it again.
            let device = try IOUSBHostDevice(__ioService: s, options: [.deviceCapture], queue: nil,
                                             interestHandler: nil)
            device.destroy()
            return nil
        } catch {
            return error.localizedDescription
        }
    }
}

/// Reads the first block of a disk, straight from the drive. macOS counts
/// it as activity, so the disk is not put to sleep. False if it failed.
func touchDisk(_ whole: String) -> Bool {
    let fd = open("/dev/r" + whole, O_RDONLY)
    guard fd >= 0 else { return false }
    defer { close(fd) }
    var block = [UInt8](repeating: 0, count: 4096)
    return block.withUnsafeMutableBytes { pread(fd, $0.baseAddress, 4096, 0) } == 4096
}

/// Asks the drive to write its volatile cache to the media
/// (DKIOCSYNCHRONIZE). Root only; call once nothing on the disk is mounted.
/// False if the drive did not confirm it.
///
/// The request goes through one of the disk's partitions: it needs a
/// descriptor open for writing, and closing the whole disk after opening it
/// for writing makes macOS re-read the partition map, which re-announces
/// every partition in the middle of the eject. The drive flushes its whole
/// cache either way.
func flushDriveCache(_ whole: String, partitions: [String]) -> Bool {
    let candidates = partitions.filter { $0 != whole } + (partitions.contains(whole) ? [whole] : [])
    for name in candidates {
        let fd = open("/dev/r" + name, O_RDWR)
        guard fd >= 0 else { continue }
        // dk_synchronize_t: offset, length, options, reserved - all zero
        // means "everything".
        var request = [UInt8](repeating: 0, count: 24)
        let DKIOCSYNCHRONIZE: UInt = 0x8018_6416   // _IOW('d', 22, dk_synchronize_t)
        let rc = request.withUnsafeMutableBytes { ioctl(fd, DKIOCSYNCHRONIZE, $0.baseAddress!) }
        close(fd)
        if rc == 0 { return true }
    }
    return false
}
