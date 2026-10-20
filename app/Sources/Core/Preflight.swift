import Darwin
import Foundation
import os

/// Checks that decide whether the PS3 core can run in this process at all, done before it
/// is loaded. Each one maps to a crash that would otherwise happen inside dlopen() or at boot.
struct Preflight {
    struct Check: Identifiable {
        let id: String
        let title: String
        let passed: Bool
        let required: Bool
        let detail: String
    }

    let jitEnabled: Bool
    let addressSpaceGB: Int
    let physicalMemory: UInt64
    let memoryLimit: UInt64

    /// RPCS3 reserves 8 GB (guest memory + mirror), 12 GB (PPU executable map) and 4 GB
    /// (stats) of address space, plus JIT arenas. Without the hook area the patched core
    /// needs about 26 GB in one process.
    static let requiredAddressSpaceGB = 28

    var checks: [Check] {
        [
            Check(
                id: "jit",
                title: "JIT",
                passed: jitEnabled,
                required: true,
                detail: jitEnabled
                    ? "The process is debugged (CS_DEBUGGED), so code pages can be made executable."
                    : "JIT is not enabled. Launch Ips3 from LiveContainer with \"Launch with JIT\" (StikDebug, SideStore or JitStreamer) and try again."
            ),
            Check(
                id: "va",
                title: "Virtual address space",
                passed: addressSpaceGB >= Preflight.requiredAddressSpaceGB,
                required: true,
                detail: addressSpaceGB >= Preflight.requiredAddressSpaceGB
                    ? "\(addressSpaceGB) GB available."
                    : "Only \(addressSpaceGB) GB could be reserved; the PS3 core needs \(Preflight.requiredAddressSpaceGB) GB. The app hosting Ips3 (LiveContainer) must be signed with the com.apple.developer.kernel.extended-virtual-addressing entitlement. This is the crash other iOS PS3 ports hit when loading the core."
            ),
            Check(
                id: "mem",
                title: "Memory limit",
                passed: memoryLimit >= 1800 << 20,
                required: false,
                detail: "\(physicalMemory >> 20) MB RAM, \(memoryLimit >> 20) MB available to this app. "
                    + (memoryLimit >= 2800 << 20
                        ? "Good."
                        : "Sign the host app with com.apple.developer.kernel.increased-memory-limit for more headroom; the low-memory profile is applied either way.")
            ),
        ]
    }

    var canRun: Bool { checks.allSatisfy { $0.passed || !$0.required } }

    static func run() -> Preflight {
        Preflight(
            jitEnabled: isJITEnabled(),
            addressSpaceGB: probeAddressSpaceGB(),
            physicalMemory: ProcessInfo.processInfo.physicalMemory,
            memoryLimit: currentMemoryLimit()
        )
    }

    // MARK: - Probes

    /// CS_DEBUGGED is what every iOS JIT enabler (StikDebug, SideStore, JitStreamer, Xcode)
    /// ultimately sets; with it the kernel lets RW pages become RX.
    static func isJITEnabled() -> Bool {
        var flags: UInt32 = 0
        let csDebugged: UInt32 = 0x1000_0000
        guard csops(getpid(), 0, &flags, MemoryLayout<UInt32>.size) == 0 else { return false }
        return flags & csDebugged != 0
    }

    /// The largest PROT_NONE reservation that succeeds, in GB, probing down from 40.
    static func probeAddressSpaceGB() -> Int {
        var gb = 40
        while gb > 0 {
            let size = gb << 30
            let pointer = mmap(nil, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0)
            if pointer != MAP_FAILED, let pointer {
                munmap(pointer, size)
                return gb
            }
            gb -= 2
        }
        return 0
    }

    /// The jetsam limit is what kills the app, and os_proc_available_memory() is the only
    /// public view of it: what is left plus what is in use.
    static func currentMemoryLimit() -> UInt64 {
        let available = UInt64(os_proc_available_memory())
        return available + residentFootprint()
    }

    static func residentFootprint() -> UInt64 {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return result == KERN_SUCCESS ? UInt64(info.phys_footprint) : 0
    }
}

@_silgen_name("csops")
private func csops(_ pid: pid_t, _ ops: UInt32, _ useraddr: UnsafeMutableRawPointer?, _ usersize: Int) -> Int32
