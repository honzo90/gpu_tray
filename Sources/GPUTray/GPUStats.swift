import Foundation
import IOKit

/// Migawka statystyk GPU odczytana z rejestru IOKit (Apple Silicon).
/// Źródło: klasa `AGXAccelerator` → słownik `PerformanceStatistics`.
/// Nie wymaga uprawnień root — zweryfikowane na Mac Studio M1 Max.
struct GPUSample: Sendable {
    let deviceUtil: Double      // "Device Utilization %"
    let rendererUtil: Double    // "Renderer Utilization %"
    let tilerUtil: Double       // "Tiler Utilization %"
    let inUseMemory: Int64      // "In use system memory" (bajty)
    let allocMemory: Int64      // "Alloc system memory" (bajty)
    let date: Date
}

enum GPUReader {
    /// Odczytuje aktualne statystyki GPU. Zwraca nil, gdy nie znaleziono klasy AGXAccelerator.
    static func read() -> GPUSample? {
        let matching = IOServiceMatching("AGXAccelerator")
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else {
            return nil
        }
        defer { IOObjectRelease(iterator) }

        var sample: GPUSample?
        var service: io_object_t = IOIteratorNext(iterator)
        while service != 0 {
            defer {
                IOObjectRelease(service)
                service = IOIteratorNext(iterator)
            }
            var props: Unmanaged<CFMutableDictionary>?
            guard IORegistryEntryCreateCFProperties(service, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS,
                  let dict = (props?.takeRetainedValue() as NSDictionary?) as? [String: Any],
                  let stats = dict["PerformanceStatistics"] as? [String: Any] else { continue }

            func num(_ key: String) -> Double {
                (stats[key] as? NSNumber)?.doubleValue ?? 0
            }
            sample = GPUSample(
                deviceUtil: num("Device Utilization %"),
                rendererUtil: num("Renderer Utilization %"),
                tilerUtil: num("Tiler Utilization %"),
                inUseMemory: Int64((stats["In use system memory"] as? NSNumber)?.int64Value ?? 0),
                allocMemory: Int64((stats["Alloc system memory"] as? NSNumber)?.int64Value ?? 0),
                date: Date()
            )
            break
        }
        return sample
    }
}

/// Pamięć zintegrowana hosta (unified memory).
enum HostMemory {
    /// (użyta: aktywna + wired + compressed, całkowita).
    static func read() -> (used: Int64, total: Int64)? {
        var total: Int64 = 0
        var len = MemoryLayout<Int64>.size
        guard sysctlbyname("hw.memsize", &total, &len, nil, 0) == 0, total > 0 else { return nil }

        var stats = vm_statistics64_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<vm_statistics64_data_t>.stride / MemoryLayout<integer_t>.stride)
        let kr = withUnsafeMutablePointer(to: &stats) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &count)
            }
        }
        guard kr == KERN_SUCCESS else { return nil }

        var pageSize: vm_size_t = 0
        _ = host_page_size(mach_host_self(), &pageSize)
        let ps = Int64(pageSize)
        let totalPages = total / ps
        let freeish = Int64(stats.free_count) + Int64(stats.inactive_count) + Int64(stats.speculative_count)
        let usedPages = totalPages - freeish
        return (used: max(usedPages, 0) * ps, total: total)
    }
}
