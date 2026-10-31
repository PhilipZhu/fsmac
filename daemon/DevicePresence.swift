// FSMac daemon: the physical device behind a disk, and flushing its cache.
//
// After an eject, many USB drives stay attached until they are unplugged,
// and the ones that do go away leave the USB device behind until the cable
// is pulled. Watching that device lets FSMac show "safe to unplug" for
// exactly as long as the drive is still connected.

import Foundation
import IOKit

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
