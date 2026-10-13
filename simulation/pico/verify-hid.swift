// Independent observer: reads the operating system's HID devices and events.
// From the device-gateway repository:
// Prepare: mkdir -p .simulation/pico/module-cache
// Compile: xcrun swiftc -module-cache-path .simulation/pico/module-cache simulation/pico/verify-hid.swift -o .simulation/pico/verify-hid
// List:    .simulation/pico/verify-hid
// Watch:   .simulation/pico/verify-hid --watch --timeout 15 [--serial OSG-PICO-...]
// Pico:    use its OSG-PICO-... gateway serial; usbInterfaceNumber maps each HID to discovery's hidInterface.
import Foundation
import IOKit
import IOKit.hid
import Darwin

private let picoVendor = 0xCAFE
private let picoProduct = 0x4010

private func emit(_ object: [String: Any]) {
    guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else { return }
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data([0x0A]))
}

private func property(_ device: IOHIDDevice, _ key: String) -> Any? {
    IOHIDDeviceGetProperty(device, key as CFString)
}

private func registryID(_ device: IOHIDDevice) -> UInt64 {
    var id: UInt64 = 0
    IORegistryEntryGetRegistryEntryID(IOHIDDeviceGetService(device), &id)
    return id
}

private func isPico(_ device: IOHIDDevice, serial: String?) -> Bool {
    guard let vendor = property(device, kIOHIDVendorIDKey) as? NSNumber,
          let product = property(device, kIOHIDProductIDKey) as? NSNumber,
          let deviceSerial = property(device, kIOHIDSerialNumberKey) as? String else { return false }
    let pico = vendor.intValue == picoVendor && product.intValue == picoProduct &&
        deviceSerial.hasPrefix("OSG-PICO-") &&
        (property(device, kIOHIDPrimaryUsagePageKey) as? NSNumber)?.intValue == 1 &&
        (property(device, kIOHIDPrimaryUsageKey) as? NSNumber)?.intValue == 4
    return pico && (serial == nil || deviceSerial == serial)
}

private func usbMetadata(_ device: IOHIDDevice) -> [String: Any] {
    // A composite USB device shares its serial across HID interfaces. Walk the
    // IOService parents to recover the interface and physical location keys.
    let keys = ["bInterfaceNumber": "usbInterfaceNumber", "locationID": "usbLocationID",
                "USB Serial Number": "usbSerialNumber", "USB Product Name": "usbProduct",
                "kUSBString": "usbInterfaceName", "iInterface": "usbInterfaceStringIndex",
                "bcdDevice": "usbDeviceVersion"]
    var result: [String: Any] = [:]
    var entry = IOHIDDeviceGetService(device)
    guard entry != 0 else { return result }
    IOObjectRetain(entry)
    defer { IOObjectRelease(entry) }
    for _ in 0..<32 {
        for (key, outputKey) in keys where result[outputKey] == nil {
            if let value = IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue(),
               value is NSNumber || value is String {
                result[outputKey] = value
            }
        }
        var parent: io_registry_entry_t = 0
        guard IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) == kIOReturnSuccess else { break }
        IOObjectRelease(entry)
        entry = parent
    }
    return result
}

private func deviceDescription(_ device: IOHIDDevice) -> [String: Any] {
    let elements = IOHIDDeviceCopyMatchingElements(device, nil, IOOptionBits(kIOHIDOptionsTypeNone)) as? [IOHIDElement] ?? []
    let inputs = elements.filter {
        let type = IOHIDElementGetType($0)
        return type == kIOHIDElementTypeInput_Misc || type == kIOHIDElementTypeInput_Button || type == kIOHIDElementTypeInput_Axis
    }
    let buttons = inputs.filter { IOHIDElementGetUsagePage($0) == 0x09 }
    let axes = inputs.filter {
        IOHIDElementGetUsagePage($0) == 0x01 && (0x30...0x38).contains(IOHIDElementGetUsage($0))
    }
    let axisDetails: [[String: Any]] = axes.map {
        ["usage": IOHIDElementGetUsage($0), "logicalMinimum": IOHIDElementGetLogicalMin($0),
         "logicalMaximum": IOHIDElementGetLogicalMax($0), "reportBits": IOHIDElementGetReportSize($0)]
    }
    var result: [String: Any] = [
        "registryID": registryID(device),
        "stableSerial": property(device, kIOHIDSerialNumberKey) as? String ?? "",
        "product": property(device, kIOHIDProductKey) as? String ?? "",
        "vendorID": property(device, kIOHIDVendorIDKey) as? NSNumber ?? 0,
        "productID": property(device, kIOHIDProductIDKey) as? NSNumber ?? 0,
        "usagePage": property(device, kIOHIDPrimaryUsagePageKey) as? NSNumber ?? 0,
        "usage": property(device, kIOHIDPrimaryUsageKey) as? NSNumber ?? 0,
        "buttonCount": buttons.count,
        "axisCount": axes.count,
        "axes": axisDetails,
    ]
    result.merge(usbMetadata(device)) { current, _ in current }
    return result
}

private final class DeviceMonitor {
    let device: IOHIDDevice
    let buffer: UnsafeMutablePointer<UInt8>
    let capacity: Int
    unowned let state: WatchState

    init(device: IOHIDDevice, state: WatchState) {
        self.device = device
        self.state = state
        capacity = max(64, (property(device, kIOHIDMaxInputReportSizeKey) as? NSNumber)?.intValue ?? 64)
        buffer = .allocate(capacity: capacity)
        buffer.initialize(repeating: 0, count: capacity)
    }

    deinit { buffer.deallocate() }
}

private final class WatchState {
    let serial: String?
    var monitors: [UInt64: DeviceMonitor] = [:]
    var reportCount = 0
    var valueCount = 0

    init(serial: String?) { self.serial = serial }

    func matched(_ device: IOHIDDevice) {
        guard isPico(device, serial: serial) else { return }
        let id = registryID(device)
        guard monitors[id] == nil else { return }
        let monitor = DeviceMonitor(device: device, state: self)
        monitors[id] = monitor
        IOHIDDeviceRegisterInputReportCallback(device, monitor.buffer, monitor.capacity, reportCallback,
                                               Unmanaged.passUnretained(monitor).toOpaque())
        emit(["event": "matched", "device": deviceDescription(device)])
    }

    func removed(_ device: IOHIDDevice) {
        guard isPico(device, serial: serial) else { return }
        emit(["event": "removed", "registryID": registryID(device),
              "stableSerial": property(device, kIOHIDSerialNumberKey) as? String ?? ""])
        // Keep callback contexts and buffers alive until the manager has closed.
    }
}

private let matchedCallback: IOHIDDeviceCallback = { context, result, _, device in
    guard result == kIOReturnSuccess, let context else { return }
    Unmanaged<WatchState>.fromOpaque(context).takeUnretainedValue().matched(device)
}

private let removedCallback: IOHIDDeviceCallback = { context, _, _, device in
    guard let context else { return }
    Unmanaged<WatchState>.fromOpaque(context).takeUnretainedValue().removed(device)
}

private let reportCallback: IOHIDReportCallback = { context, result, _, type, reportID, bytes, length in
    guard result == kIOReturnSuccess, let context, type == kIOHIDReportTypeInput else { return }
    let monitor = Unmanaged<DeviceMonitor>.fromOpaque(context).takeUnretainedValue()
    monitor.state.reportCount += 1
    emit(["event": "inputReport", "registryID": registryID(monitor.device),
          "stableSerial": property(monitor.device, kIOHIDSerialNumberKey) as? String ?? "",
          "reportID": reportID, "bytes": Array(UnsafeBufferPointer(start: bytes, count: length)),
          "hex": Data(bytes: bytes, count: length).map { String(format: "%02x", $0) }.joined()])
}

private let valueCallback: IOHIDValueCallback = { context, result, _, value in
    guard result == kIOReturnSuccess, let context else { return }
    let state = Unmanaged<WatchState>.fromOpaque(context).takeUnretainedValue()
    let element = IOHIDValueGetElement(value)
    let device = IOHIDElementGetDevice(element)
    guard isPico(device, serial: state.serial) else { return }
    state.valueCount += 1
    emit(["event": "element", "registryID": registryID(device),
          "stableSerial": property(device, kIOHIDSerialNumberKey) as? String ?? "",
          "usagePage": IOHIDElementGetUsagePage(element), "usage": IOHIDElementGetUsage(element),
          "value": IOHIDValueGetIntegerValue(value), "reportID": IOHIDElementGetReportID(element)])
}

var watch = false
var timeout = 15.0
var selectedSerial: String?
var args = Array(CommandLine.arguments.dropFirst())
while !args.isEmpty {
    let argument = args.removeFirst()
    switch argument {
    case "--watch": watch = true
    case "--timeout":
        guard !args.isEmpty, let seconds = Double(args.removeFirst()), seconds > 0, seconds <= 300 else {
            emit(["error": "--timeout must be between 0 and 300 seconds"]); exit(2)
        }
        timeout = seconds
    case "--serial":
        guard !args.isEmpty else { emit(["error": "--serial requires a HID serial number"]); exit(2) }
        selectedSerial = args.removeFirst()
    case "--help":
        print("verify-hid [--watch] [--timeout SECONDS] [--serial SERIAL_NUMBER]\nPico: OSG-PICO-... (all module interfaces share the gateway serial).\nReads Pico joystick interfaces 0xCAFE:0x4010 with OSG-PICO- serials.\nMap usbInterfaceNumber to the gateway discovery snapshot's hidInterface field.")
        exit(0)
    default: emit(["error": "Unknown argument: \(argument)"]); exit(2)
    }
}

let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
let matching = [[kIOHIDVendorIDKey: picoVendor, kIOHIDProductIDKey: picoProduct,
                 kIOHIDPrimaryUsagePageKey: 1, kIOHIDPrimaryUsageKey: 4]]
IOHIDManagerSetDeviceMatchingMultiple(manager, matching as CFArray)
private let state = WatchState(serial: selectedSerial)
if watch {
    let context = Unmanaged.passUnretained(state).toOpaque()
    IOHIDManagerRegisterDeviceMatchingCallback(manager, matchedCallback, context)
    IOHIDManagerRegisterDeviceRemovalCallback(manager, removedCallback, context)
    IOHIDManagerRegisterInputValueCallback(manager, valueCallback, context)
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
}
let openResult = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
guard openResult == kIOReturnSuccess else {
    emit(["event": "error", "operation": "IOHIDManagerOpen", "code": openResult,
          "message": "macOS could not open the HID observer. Check Input Monitoring permission if access was denied."])
    exit(1)
}
let devices = (IOHIDManagerCopyDevices(manager) as? Set<IOHIDDevice> ?? [])
    .filter { isPico($0, serial: selectedSerial) }
    .sorted { registryID($0) < registryID($1) }
emit(["event": "snapshot", "count": devices.count, "devices": devices.map(deviceDescription)])
if watch {
    for device in devices { state.matched(device) }
    emit(["event": "watching", "timeoutSeconds": timeout])
    RunLoop.current.run(until: Date().addingTimeInterval(timeout))
    emit(["event": "summary", "reports": state.reportCount, "elementUpdates": state.valueCount])
    IOHIDManagerUnscheduleFromRunLoop(manager, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
}
IOHIDManagerClose(manager, IOOptionBits(kIOHIDOptionsTypeNone))
withExtendedLifetime(state) {}
