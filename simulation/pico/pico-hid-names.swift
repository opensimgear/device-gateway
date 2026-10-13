// macOS's USB HID driver uses the shared USB iProduct for composite controllers.
// Publish module names on the actual registry services, rather than on an
// IOHIDDevice client cache. This affects this Mac until USB re-enumeration.
// Input: {gatewaySerial, modules: [{id, hidInterface, label}]} on stdin.
import Foundation
import IOKit
import IOKit.hid
import Darwin

private struct ModuleName: Decodable {
    let id: String
    let hidInterface: Int
    let label: String
}

private struct Request: Decodable {
    let gatewaySerial: String
    let modules: [ModuleName]
}

private func emit(_ value: [String: Any]) {
    guard let data = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]),
          data.count <= 16_384 else {
        FileHandle.standardOutput.write(Data("{\"ok\":false,\"devices\":[],\"error\":\"Cannot encode HID naming result.\"}\n".utf8))
        return
    }
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data([10]))
}

private func valid(_ request: Request) -> Bool {
    guard request.gatewaySerial.range(of: "\\AOSG-PICO-[A-Za-z0-9_-]{4,64}\\z", options: .regularExpression) != nil,
          request.modules.count <= 8,
          Set(request.modules.map(\.id)).count == request.modules.count,
          Set(request.modules.map(\.hidInterface)).count == request.modules.count else { return false }
    return request.modules.allSatisfy {
        $0.id.range(of: "\\A[A-Za-z0-9_-]{1,40}\\z", options: .regularExpression) != nil &&
        (2...9).contains($0.hidInterface) &&
        !$0.label.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        $0.label.unicodeScalars.count <= 80 &&
        !$0.label.unicodeScalars.contains { $0.value < 32 || (127...159).contains($0.value) }
    }
}

private func property(_ service: io_registry_entry_t, _ key: String) -> Any? {
    IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
}

private func number(_ service: io_registry_entry_t, _ key: String) -> Int? {
    (property(service, key) as? NSNumber)?.intValue
}

private func interface(_ service: io_registry_entry_t) -> Int? {
    var entry = service
    IOObjectRetain(entry)
    defer { IOObjectRelease(entry) }
    for _ in 0..<16 {
        if let value = number(entry, "bInterfaceNumber") { return value }
        var parent: io_registry_entry_t = 0
        guard IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) == kIOReturnSuccess else { return nil }
        IOObjectRelease(entry)
        entry = parent
    }
    return nil
}

private func registryID(_ entry: io_registry_entry_t) -> String {
    var value: UInt64 = 0
    IORegistryEntryGetRegistryEntryID(entry, &value)
    return String(value)
}

// Read registry identity, never trust a label or the payload's module id alone.
private func isGatewayHID(_ service: io_registry_entry_t, serial: String) -> Bool {
    number(service, kIOHIDVendorIDKey) == 0xCAFE &&
    number(service, kIOHIDProductIDKey) == 0x4010 &&
    number(service, kIOHIDPrimaryUsagePageKey) == 1 &&
    number(service, kIOHIDPrimaryUsageKey) == 4 &&
    property(service, kIOHIDSerialNumberKey) as? String == serial
}

private func setName(_ service: io_registry_entry_t, _ label: String) -> [String: Any] {
    let before = property(service, kIOHIDProductKey) as? String ?? ""
    let result = before == label ? kIOReturnSuccess :
        IORegistryEntrySetCFProperty(service, kIOHIDProductKey as CFString, label as CFString)
    // IOHIDDeviceSetProperty would update only the calling object's cache. Read
    // IORegistry again so that a successful client-local write cannot pass.
    let actual = property(service, kIOHIDProductKey) as? String ?? ""
    var record: [String: Any] = ["registryID": registryID(service), "product": actual,
                                "ok": result == kIOReturnSuccess && actual == label]
    if result != kIOReturnSuccess {
        record["error"] = "macOS rejected this HID service name (\(result))."
    } else if actual != label {
        record["error"] = "macOS retained the cached HID service name."
    }
    return record
}

private func descendantNames(_ service: io_registry_entry_t, _ request: Request,
                             _ module: ModuleName, depth: Int = 0) -> [[String: Any]] {
    guard depth < 3 else { return [] }
    var iterator: io_iterator_t = 0
    guard IORegistryEntryGetChildIterator(service, kIOServicePlane, &iterator) == kIOReturnSuccess else { return [] }
    defer { IOObjectRelease(iterator) }
    var result: [[String: Any]] = []
    var inspected = 0
    while result.count < 8 && inspected < 32 {
        let child = IOIteratorNext(iterator)
        guard child != 0 else { break }
        inspected += 1
        defer { IOObjectRelease(child) }
        // Only name the two HID descendants belonging to the already verified
        // module. Ignore clients, USB ancestors, and unrelated registry nodes.
        let isInterface = IOObjectConformsTo(child, "IOHIDInterface") != 0
        let isEventService = IOObjectConformsTo(child, "AppleUserHIDEventService") != 0
        guard (isInterface || isEventService),
              property(child, kIOHIDSerialNumberKey) as? String == request.gatewaySerial,
              number(child, kIOHIDVendorIDKey) == 0xCAFE,
              number(child, kIOHIDProductIDKey) == 0x4010,
              interface(child) == module.hidInterface else { continue }
        var named = setName(child, module.label)
        named["class"] = isInterface ? "IOHIDInterface" : "AppleUserHIDEventService"
        result.append(named)
        result.append(contentsOf: descendantNames(child, request, module, depth: depth + 1).prefix(8 - result.count))
    }
    return result
}

if CommandLine.arguments.dropFirst() == ["--self-test"] {
    let base = Request(gatewaySerial: "OSG-PICO-E661640843565431", modules: [
        ModuleName(id: "OSG-SQ-0042", hidInterface: 2, label: "OSG Sequential shifter")])
    let bad: [Request] = [
        Request(gatewaySerial: "unrelated", modules: base.modules),
        Request(gatewaySerial: base.gatewaySerial, modules: base.modules + base.modules),
        Request(gatewaySerial: base.gatewaySerial, modules: [ModuleName(id: "panel", hidInterface: 1, label: "OSG Panel")]),
        Request(gatewaySerial: base.gatewaySerial, modules: [ModuleName(id: "panel", hidInterface: 10, label: "OSG Panel")]),
        Request(gatewaySerial: base.gatewaySerial, modules: [ModuleName(id: "../panel", hidInterface: 2, label: "OSG Panel")]),
        Request(gatewaySerial: base.gatewaySerial, modules: [ModuleName(id: "panel", hidInterface: 2, label: "\nOSG Panel")]),
        Request(gatewaySerial: base.gatewaySerial, modules: [ModuleName(id: "panel", hidInterface: 2, label: " ")]),
        Request(gatewaySerial: base.gatewaySerial, modules: [ModuleName(id: "panel", hidInterface: 2, label: String(repeating: "A", count: 81))])
    ]
    let passed = valid(base) && bad.allSatisfy { !valid($0) } &&
        valid(Request(gatewaySerial: base.gatewaySerial, modules: [])) &&
        valid(Request(gatewaySerial: base.gatewaySerial, modules: [ModuleName(id: "panel", hidInterface: 2, label: "OSG Pédales 🏁")]))
    emit(["ok": passed, "devices": [], "tests": 11])
    exit(passed ? 0 : 1)
}

guard CommandLine.arguments.count == 1 else {
    emit(["ok": false, "devices": [], "error": "HID naming accepts only stdin JSON or --self-test."])
    exit(2)
}
private func readRequest() -> Data? {
    var input = Data()
    while input.count <= 8_192 {
        do {
            guard let chunk = try FileHandle.standardInput.read(upToCount: 8_193 - input.count),
                  !chunk.isEmpty else { return input }
            input.append(chunk)
        } catch { return nil }
    }
    return nil
}
guard let input = readRequest(),
      let request = try? JSONDecoder().decode(Request.self, from: input), valid(request) else {
    emit(["ok": false, "devices": [], "error": "Invalid Pico HID naming request."])
    exit(2)
}
if request.modules.isEmpty { emit(["ok": true, "devices": []]); exit(0) }

let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, [kIOHIDVendorIDKey: 0xCAFE, kIOHIDProductIDKey: 0x4010,
    kIOHIDPrimaryUsagePageKey: 1, kIOHIDPrimaryUsageKey: 4,
    kIOHIDSerialNumberKey: request.gatewaySerial] as CFDictionary)
let opened = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
guard opened == kIOReturnSuccess else {
    emit(["ok": false, "devices": [], "error": "macOS could not open Pico HID services (\(opened))."])
    exit(1)
}
var records: [[String: Any]] = []
var failures: [String] = []
let devices = IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> ?? []
for module in request.modules {
    let services = devices.map { IOHIDDeviceGetService($0) }.filter {
        isGatewayHID($0, serial: request.gatewaySerial) && interface($0) == module.hidInterface
    }
    guard services.count == 1, let service = services.first else {
        failures.append("HID interface \(module.hidInterface) is not uniquely available.")
        records.append(["id": module.id, "hidInterface": module.hidInterface,
                        "label": module.label, "ok": false, "product": "", "descendants": []])
        continue
    }
    var named = setName(service, module.label)
    named["id"] = module.id
    named["hidInterface"] = module.hidInterface
    named["label"] = module.label
    if named["ok"] as? Bool != true {
        failures.append("macOS could not publish the name for HID interface \(module.hidInterface).")
    }
    named["descendants"] = descendantNames(service, request, module)
    records.append(named)
}
IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
var result: [String: Any] = ["ok": failures.isEmpty, "devices": records]
if !failures.isEmpty { result["error"] = failures.joined(separator: " ") }
emit(result)
exit(failures.isEmpty ? 0 : 1)
