// IrisXe Manager - a small window for installing and repairing the Iris Xe driver.
// The work is done by irisxe-manager.sh (bundled in the app's Resources); this file is only the window.
// Built by manager/build.sh (GitHub Actions puts "IrisXe Manager.app" into the build artifact's out/ folder).
import SwiftUI
import AppKit

struct Part: Identifiable {
    let id: String          // the script's name for it
    let title: String
    let note: String
    var path = ""
    var date = ""
    var chosen = false
}

struct StatusRow: Identifiable {
    let id = UUID()
    let name: String
    let value: String
    let extra: String
    var warn: Bool {
        let v = value.lowercased()
        return v.contains("not installed") || v.contains("locked") || v.contains("parked") || v.contains("missing")
            || v.hasPrefix("off") || v.contains("verbose")
    }
}

final class Model: ObservableObject {
    @Published var folder = ""
    @Published var mesaFolder = ""   // b221: the Mesa build (full Metal plugin), chosen on its own
    @Published var parts: [Part] = [
        Part(id: "gpu", title: "Display driver (NMXGPU.kext)", note: "screen, brightness, sleep - restart needed"),
        Part(id: "accel", title: "Accelerator (NMXAccel.kext)", note: "the right one for this macOS is picked - restart needed"),
        Part(id: "agdc", title: "Display helper (NVRMAGDC.kext)", note: "restart needed"),
        Part(id: "plugin", title: "Metal plugin - update (quiet)", note: "replaces only the plugin program"),
        Part(id: "fullplugin", title: "Metal plugin - full (Mesa build)", note: "first install on a new macOS, includes the shader reader"),
        Part(id: "xlate", title: "Shader translator", note: "libnvmtl_translate.dylib"),
        Part(id: "autoarm", title: "Acceleration at every boot (autoarm)", note: "comes with this app"),
    ]
    @Published var status: [StatusRow] = []
    @Published var log = ""
    @Published var busy = false

    let script: String = Bundle.main.path(forResource: "irisxe-manager", ofType: "sh") ?? ""

    init() {
        // when the app sits inside a downloaded build (out/IrisXe Manager.app), that build is the default source
        let here = Bundle.main.bundleURL.deletingLastPathComponent()
        if FileManager.default.fileExists(atPath: here.appendingPathComponent("NMXGPU.kext").path) {
            folder = here.deletingLastPathComponent().path
        }
        refresh()
        if !folder.isEmpty { scan() }
    }

    // ---- running the script ----
    private func shellQuote(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

    func run(_ args: [String], admin: Bool, done: @escaping (String) -> Void) {
        let cmd = (["/bin/bash", script] + args).map(shellQuote).joined(separator: " ") + " 2>&1"
        DispatchQueue.global(qos: .userInitiated).async {
            let p = Process()
            if admin {
                // osascript shows macOS's own password prompt; the command then runs as root
                let asCmd = cmd.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
                p.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
                p.arguments = ["-e", "do shell script \"\(asCmd)\" with administrator privileges"]
            } else {
                p.executableURL = URL(fileURLWithPath: "/bin/bash")
                p.arguments = ["-c", cmd]
            }
            let pipe = Pipe()
            p.standardOutput = pipe
            p.standardError = pipe
            var out = ""
            do {
                try p.run()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                p.waitUntilExit()
                out = String(data: data, encoding: .utf8) ?? ""
                if admin && p.terminationStatus != 0 && out.contains("-128") { out = "cancelled (no password given)\n" }
            } catch {
                out = "could not run the helper: \(error.localizedDescription)\n"
            }
            DispatchQueue.main.async { done(out) }
        }
    }

    func appendLog(_ s: String) {
        let stamp = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
        log += "[\(stamp)]\n" + s + (s.hasSuffix("\n") ? "" : "\n")
    }

    // ---- actions ----
    func refresh() {
        run(["status"], admin: false) { out in
            self.status = out.split(separator: "\n").compactMap { line in
                let f = line.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
                guard f.count >= 2 else { return nil }
                return StatusRow(name: f[0], value: f[1], extra: f.count > 2 ? f[2] : "")
            }
        }
    }

    // ---- the two downloads: the NMXGPU build (drivers, quiet plugin, translator, helper) and the Mesa ANV build ----
    // b223: each chooser accepts only its own kind of download and says which button the other one belongs to.
    private func parseScan(_ out: String) -> [String: (path: String, date: String)] {
        var r: [String: (path: String, date: String)] = [:]
        for line in out.split(separator: "\n") {
            let kv = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
            guard kv.count == 2 else { continue }
            let pd = kv[1].split(separator: "|", omittingEmptySubsequences: false).map(String.init)
            r[kv[0]] = (pd.first ?? "", pd.count > 1 ? pd[1] : "")
        }
        return r
    }

    private func warn(_ title: String, _ text: String) {
        let a = NSAlert()
        a.messageText = title
        a.informativeText = text
        a.addButton(withTitle: "OK")
        a.runModal()
    }

    private func pickFolder(_ message: String) -> String? {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.message = message
        return panel.runModal() == .OK ? panel.url?.path : nil
    }

    func chooseFolder() {
        guard let path = pickFolder("Choose the unzipped NMXGPU build (GitHub artifact nmxgpu-macos-...)") else { return }
        run(["scan", path], admin: false) { out in
            let r = self.parseScan(out)
            let hasGPU = !(r["gpu"]?.path ?? "").isEmpty
            let hasMesa = !(r["fullplugin"]?.path ?? "").isEmpty
            guard hasGPU else {
                if hasMesa {
                    self.warn("This is the Mesa ANV build", "The folder you chose is the Mesa ANV build (the full Metal plugin). Choose it with \"Choose Mesa ANV Build...\" instead.")
                } else {
                    self.warn("No NMXGPU build in this folder", "This folder has no NMXGPU.kext. Choose the unzipped NMXGPU build from GitHub (the folder that contains 'out').")
                }
                return
            }
            self.folder = path
            self.apply(r, skipFull: !self.mesaFolder.isEmpty)
        }
    }

    func chooseMesaFolder() {
        guard let path = pickFolder("Choose the unzipped Mesa ANV build (GitHub artifact nmx-vulkan-...) - it has the full Metal plugin") else { return }
        run(["scan", path], admin: false) { out in
            let r = self.parseScan(out)
            let hasGPU = !(r["gpu"]?.path ?? "").isEmpty
            let hasMesa = !(r["fullplugin"]?.path ?? "").isEmpty
            guard hasMesa else {
                if hasGPU {
                    self.warn("This is the NMXGPU build", "The folder you chose is the NMXGPU build (drivers and the plugin update). Choose it with \"Choose NMXGPU Build...\" instead.")
                } else {
                    self.warn("No Mesa ANV build in this folder", "This folder has no full Metal plugin (NVMTLDriver.bundle with its shader reader). Choose the unzipped Mesa ANV build from GitHub.")
                }
                return
            }
            self.mesaFolder = path
            if let i = self.parts.firstIndex(where: { $0.id == "fullplugin" }) {
                self.parts[i].path = r["fullplugin"]?.path ?? ""
                self.parts[i].date = r["fullplugin"]?.date ?? ""
            }
        }
    }

    // fill the parts from a scan; the full plugin comes from the Mesa build once one is chosen
    private func apply(_ r: [String: (path: String, date: String)], skipFull: Bool) {
        for i in parts.indices {
            if parts[i].id == "fullplugin" && skipFull { continue }
            parts[i].path = r[parts[i].id]?.path ?? ""
            parts[i].date = r[parts[i].id]?.date ?? ""
            if parts[i].path.isEmpty { parts[i].chosen = false }
        }
    }

    // refresh: scan both chosen folders again
    func scanMesa() {
        guard !mesaFolder.isEmpty else { return }
        run(["scan", mesaFolder], admin: false) { out in
            let r = self.parseScan(out)
            guard let i = self.parts.firstIndex(where: { $0.id == "fullplugin" }) else { return }
            self.parts[i].path = r["fullplugin"]?.path ?? ""
            self.parts[i].date = r["fullplugin"]?.date ?? ""
            if self.parts[i].path.isEmpty { self.parts[i].chosen = false }
        }
    }

    func scan() {
        guard !folder.isEmpty else { return }
        run(["scan", folder], admin: false) { out in self.apply(self.parseScan(out), skipFull: !self.mesaFolder.isEmpty) }
    }

    func chooseAllFound() {
        for i in parts.indices {
            // the full plugin is only for a first install; the quiet update is the everyday one
            parts[i].chosen = !parts[i].path.isEmpty && parts[i].id != "fullplugin"
        }
    }

    func install() {
        // the full plugin first: it replaces the whole bundle, the quiet update then replaces only its program
        let picked = parts.filter { $0.chosen && !$0.path.isEmpty }
        let ordered = picked.filter { $0.id == "fullplugin" } + picked.filter { $0.id != "fullplugin" }
        let chosen = ordered.map { ($0.id == "fullplugin" && !mesaFolder.isEmpty) ? "fullplugin@" + mesaFolder : $0.id }
        guard !chosen.isEmpty else { appendLog("Nothing chosen."); return }
        let base = folder.isEmpty ? mesaFolder : folder
        busy = true
        appendLog("Installing: " + chosen.map { $0.components(separatedBy: "@").first ?? $0 }.joined(separator: ", "))
        run(["install", base] + chosen, admin: true) { out in
            self.appendLog(out)
            self.busy = false
            self.refresh()
        }
    }

    func repair() {
        busy = true
        run(["repair"], admin: true) { out in self.appendLog(out); self.busy = false; self.refresh() }
    }

    func armNow() {
        busy = true
        run(["armnow"], admin: true) { out in self.appendLog(out); self.busy = false }
    }

    func restartMac() {
        let a = NSAlert()
        a.messageText = "Restart the Mac now?"
        a.informativeText = "Save your work first - open apps will be closed."
        a.addButton(withTitle: "Restart")
        a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        p.arguments = ["-e", "do shell script \"/sbin/shutdown -r now\" with administrator privileges"]
        try? p.run()
    }
}

struct ContentView: View {
    @StateObject private var m = Model()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Iris Xe Driver").font(.title2).bold()
                Spacer()
                if m.busy { ProgressView().controlSize(.small) }
                Button { m.refresh(); m.scan(); m.scanMesa() } label: { Label("Refresh", systemImage: "arrow.clockwise") }
            }

            GroupBox("Now") {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(m.status) { row in
                        HStack(alignment: .firstTextBaseline) {
                            Text(row.name).frame(width: 90, alignment: .leading).foregroundStyle(.secondary)
                            Text(row.value).foregroundStyle(row.warn ? Color.orange : Color.primary)
                            if !row.extra.isEmpty { Text(row.extra).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail) }
                            Spacer()
                        }
                        .font(.system(.callout, design: .default))
                    }
                    if m.status.isEmpty { Text("reading...").foregroundStyle(.secondary) }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(4)
            }

            GroupBox("Install from the downloaded builds") {
                VStack(alignment: .leading, spacing: 6) {
                    HStack(alignment: .firstTextBaseline) {
                        Text("NMXGPU build").bold().frame(width: 120, alignment: .leading)
                        Text(m.folder.isEmpty ? "not chosen - drivers, plugin update, translator, helper" : (m.folder as NSString).lastPathComponent)
                            .lineLimit(1).truncationMode(.middle).foregroundStyle(m.folder.isEmpty ? .secondary : .primary)
                            .help(m.folder)
                        Spacer()
                        Button("Choose NMXGPU Build...") { m.chooseFolder() }
                    }
                    HStack(alignment: .firstTextBaseline) {
                        Text("Mesa ANV build").bold().frame(width: 120, alignment: .leading)
                        Text(m.mesaFolder.isEmpty ? "not chosen - full Metal plugin, only needed on a new macOS" : (m.mesaFolder as NSString).lastPathComponent)
                            .lineLimit(1).truncationMode(.middle).foregroundStyle(m.mesaFolder.isEmpty ? .secondary : .primary)
                            .help(m.mesaFolder)
                        Spacer()
                        Button("Choose Mesa ANV Build...") { m.chooseMesaFolder() }
                    }
                    Divider()
                    ForEach($m.parts) { $p in
                        HStack(alignment: .firstTextBaseline) {
                            Toggle(isOn: $p.chosen) {
                                VStack(alignment: .leading, spacing: 1) {
                                    Text(p.title)
                                    Text(p.path.isEmpty ? "not in this folder" : "\(p.note)  -  \(p.date)")
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                            }
                            .disabled(p.path.isEmpty)
                            Spacer()
                        }
                    }
                    HStack {
                        Button("Choose Everything Found") { m.chooseAllFound() }.disabled(m.folder.isEmpty)
                        Spacer()
                        Button("Install Selected") { m.install() }
                            .keyboardShortcut(.defaultAction)
                            .disabled(m.busy || !m.parts.contains { $0.chosen })
                    }
                }
                .padding(4)
            }

            GroupBox("Fix") {
                HStack {
                    Button("Clear Safety Locks") { m.repair() }.help("After a hang: lets the display driver and acceleration start again at the next boot")
                    Button("Turn Acceleration On Now") { m.armNow() }.help("Restarts the desktop - open apps close")
                    Spacer()
                    Button("Restart Mac") { m.restartMac() }
                }
                .disabled(m.busy)
                .padding(4)
            }

            GroupBox("Log") {
                ScrollView {
                    Text(m.log.isEmpty ? "Nothing done yet." : m.log)
                        .font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
                .frame(minHeight: 120)
            }
        }
        .padding(16)
        .frame(minWidth: 620, minHeight: 720)
    }
}

@main
struct IrisXeManagerApp: App {
    var body: some Scene {
        WindowGroup("IrisXe Manager") { ContentView() }
            .windowResizability(.contentMinSize)
    }
}
