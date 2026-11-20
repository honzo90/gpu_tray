import Foundation
import Darwin

/// Ruch sieciowy: liczniki bajtów interfejsów `en*` (UP, bez loopback) z sysctl `NET_RT_IFLIST2`.
/// Apple Silicon: en0 = wbudowany ethernet, en1 = Wi-Fi — sumujemy oba.
/// Uwaga: niezsynchronizowany stan — wołać z jednego wątku (tickSecond / --once).
final class NetMonitor {
    private var prev: (inB: UInt64, outB: UInt64, at: Date)?
    private var lastRate: (down: Double, up: Double)?   // MB/s

    /// Wywoływać co ~1 s. Zwraca prędkości chwilowe (MB/s dziesiętnie — jak Monitor aktywności).
    func update() -> (down: Double, up: Double)? {
        guard let tot = Self.readTotals() else { return lastRate }
        defer { prev = (tot.inB, tot.outB, Date()) }
        guard let p = prev else { return lastRate }
        let dt = Date().timeIntervalSince(p.at)
        guard dt > 0.2 else { return lastRate }
        // licznik mógł spaść (restart interfejsu) → czekamy na nowy punkt odniesienia
        guard tot.inB >= p.inB, tot.outB >= p.outB else { return lastRate }
        lastRate = (Double(tot.inB - p.inB) / dt / 1_000_000,
                    Double(tot.outB - p.outB) / dt / 1_000_000)
        return lastRate
    }

    /// Sumaryczne bajty przyjęte/wysłane przez wszystkie interfejsy `en*` od startu systemu.
    private static func readTotals() -> (inB: UInt64, outB: UInt64)? {
        // mib ma 6 elementów — szósty (ifindex=0) jest częścią nazwy sysctl!
        var mib: [Int32] = [CTL_NET, AF_ROUTE, 0, 0, NET_RT_IFLIST2, 0]
        var size: Int = 0
        guard sysctl(&mib, 6, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var buf = [UInt8](repeating: 0, count: size)
        guard sysctl(&mib, 6, &buf, &size, nil, 0) == 0, size > 0 else { return nil }

        let hdrSize = MemoryLayout<if_msghdr2>.size
        var inB: UInt64 = 0
        var outB: UInt64 = 0

        // Bufor to mieszanka typów komunikatów o różnych nagłówkach. Wspólny początek
        // to msglen(2B, host order) + version(1B) + type(1B) — tylko tyle czytamy na
        // starcie; pełny if_msghdr2 kopiujemy wyłącznie dla RTM_IFINFO2.
        let ok = buf.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Bool in
            var off = 0
            while off + 4 <= raw.count {
                let b0 = raw.load(fromByteOffset: off, as: UInt8.self)
                let b1 = raw.load(fromByteOffset: off + 1, as: UInt8.self)
                let msgLen = Int(b0) | (Int(b1) << 8)
                let msgType = raw.load(fromByteOffset: off + 3, as: UInt8.self)
                guard msgLen > 0 else { return false }
                defer { off += msgLen }

                guard msgType == UInt8(RTM_IFINFO2), off + hdrSize <= raw.count else { continue }

                var hdr = if_msghdr2()
                memcpy(&hdr, raw.baseAddress! + off, hdrSize)
                guard hdr.ifm_flags & Int32(IFF_UP) != 0,
                      hdr.ifm_flags & Int32(IFF_LOOPBACK) == 0 else { continue }

                // nazwa: sockaddr_dl zaraz za nagłówkiem; sdl_nlen na offset 5, sdl_data na 8
                let nameOff = off + hdrSize + 8
                let nameLen = min(Int(raw.load(fromByteOffset: off + hdrSize + 5, as: UInt8.self)), 16)
                guard nameLen > 0, nameOff + nameLen <= raw.count else { continue }
                let name = String(bytes: raw[nameOff ..< nameOff + nameLen], encoding: .utf8) ?? ""
                guard name.hasPrefix("en") else { continue }

                inB += hdr.ifm_data.ifi_ibytes
                outB += hdr.ifm_data.ifi_obytes
            }
            return true
        }
        guard ok else { return nil }
        return (inB, outB)
    }
}

/// Użycie CPU (wszystkie rdzenie) — delta ticków `PROCESSOR_CPU_LOAD_INFO`.
/// Uwaga: jak NetMonitor — wołać z jednego wątku.
final class CpuMonitor {
    private var prev: (total: UInt64, idle: UInt64)?

    /// Wywoływać co ~1 s. Procent 0…100.
    func update() -> Double? {
        var numCPUs: natural_t = 0
        var info: processor_info_array_t?
        var infoCount: mach_msg_type_number_t = 0
        let kr = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &numCPUs, &info, &infoCount)
        guard kr == KERN_SUCCESS, let info else { return nil }
        let size = vm_size_t(infoCount) * vm_size_t(MemoryLayout<integer_t>.size)
        defer { vm_deallocate(mach_task_self_, vm_address_t(UInt(bitPattern: info)), size) }

        var total: UInt64 = 0
        var idle: UInt64 = 0
        for c in 0..<Int(numCPUs) {
            // CPU_STATE_USER, CPU_STATE_SYSTEM, CPU_STATE_IDLE, CPU_STATE_NICE
            let user = UInt64(info[c * 4])
            let sys = UInt64(info[c * 4 + 1])
            let idl = UInt64(info[c * 4 + 2])
            let nice = UInt64(info[c * 4 + 3])
            total += user + sys + idl + nice
            idle += idl + nice
        }
        defer { prev = (total, idle) }
        guard let p = prev else { return nil }
        guard total > p.total else { return nil }
        return min(max(Double(total - p.total - (idle - p.idle)) / Double(total - p.total) * 100, 0), 100)
    }
}
