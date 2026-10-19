import SwiftUI

struct TabBar: View {
    @EnvironmentObject private var manager: SessionManager
    @EnvironmentObject private var book: AddressBook

    var body: some View {
        HStack(spacing: 0) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 4) {
                    ForEach(manager.sessions) { session in
                        TabItem(session: session, isSelected: session.id == manager.selectedID)
                    }
                }
                .padding(.horizontal, 6)
                .padding(.vertical, 4)
            }
            Menu {
                if book.entries.isEmpty {
                    Text("Adresboek is leeg")
                }
                ForEach(book.groups, id: \.self) { group in
                    Section(group.isEmpty ? "Overige" : group) {
                        ForEach(book.entries(in: group)) { entry in
                            Button(entry.displayName) { Task { await manager.connect(entry) } }
                        }
                    }
                }
                Divider()
                Button("Nieuwe host…") { NotificationCenter.default.post(name: .newHost, object: nil) }
            } label: {
                Image(systemName: "plus")
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
            .help("Nieuwe tab")
            .padding(.horizontal, 8)

            Button {
                NSApp.keyWindow?.toggleFullScreen(nil)
            } label: {
                Image(systemName: manager.focusMode
                    ? "arrow.down.right.and.arrow.up.left" : "arrow.up.left.and.arrow.down.right")
            }
            .buttonStyle(.borderless)
            .help(manager.focusMode ? "Volledig scherm verlaten (⌃⌘F)" : "Volledig scherm (⌃⌘F)")
            .padding(.trailing, 10)
        }
        .frame(height: 34)
    }
}

private struct TabItem: View {
    @EnvironmentObject private var manager: SessionManager
    @ObservedObject var session: RDPSession
    let isSelected: Bool
    @State private var hovering = false

    var body: some View {
        HStack(spacing: 6) {
            StatusDot(status: session.status)
            Text(session.entry.displayName)
                .lineLimit(1)
            if session.aiEnabled {
                Image(systemName: "sparkles")
                    .font(.caption2)
                    .foregroundStyle(.orange)
                    .help("AI-besturing staat aan")
            }
            Button {
                manager.close(session)
            } label: {
                Image(systemName: "xmark")
                    .font(.caption2.weight(.semibold))
            }
            .buttonStyle(.borderless)
            .opacity(hovering || isSelected ? 1 : 0)
            .help("Tab sluiten (⌥⌘W)")
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isSelected ? Color.accentColor.opacity(0.18) : hovering ? Color.primary.opacity(0.06) : .clear)
        )
        .contentShape(Rectangle())
        .onTapGesture { manager.selectedID = session.id }
        .onHover { hovering = $0 }
        .contextMenu {
            Button("Opnieuw verbinden") { Task { await manager.reconnect(session) } }
            Button(session.aiEnabled ? "AI-besturing uitzetten" : "AI-besturing aanzetten") {
                session.setAI(!session.aiEnabled)
            }
            Divider()
            Button("Tab sluiten") { manager.close(session) }
        }
    }
}

struct StatusDot: View {
    let status: RDPSession.Status

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 8, height: 8)
    }

    private var color: Color {
        switch status {
        case .connected: return .green
        case .connecting: return .yellow
        case .disconnected: return .gray
        case .failed: return .red
        }
    }
}

struct StatusBar: View {
    @ObservedObject var session: RDPSession

    var body: some View {
        HStack(spacing: 12) {
            StatusDot(status: session.status)
            Text(session.entry.displayName)
                .fontWeight(.medium)
            if session.entry.displayName != session.entry.host {
                Text(session.entry.host).foregroundStyle(.secondary)
            }
            if session.remoteSize != .zero {
                Text("\(Int(session.remoteSize.width)) × \(Int(session.remoteSize.height))")
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
            Spacer()
            if let notice = session.notice {
                Text(notice)
                    .foregroundStyle(.orange)
                    .transition(.opacity)
            }
            Toggle(isOn: Binding(get: { session.aiEnabled }, set: { session.setAI($0) })) {
                Label("AI-besturing", systemImage: "sparkles")
            }
            .toggleStyle(.switch)
            .controlSize(.mini)
            .help("AI-besturing aan/uit (⌃⌥⇧A in de sessie)")
        }
        .font(.callout)
        .padding(.horizontal, 12)
        .frame(height: 26)
        .animation(.default, value: session.notice)
    }
}
