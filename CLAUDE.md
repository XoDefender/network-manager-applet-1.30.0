# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

NetworkManager Applet (nm-applet) is a GNOME desktop application providing a graphical interface for NetworkManager. It consists of two main components:
- **nm-applet**: System tray applet for network status and quick connection management
- **nm-connection-editor**: Full-featured connection editor for managing network configurations

Version: 1.30.0 (based on upstream 1.20.0 with custom modifications)
License: GPL-2.0+

## Build System

This project supports both **Meson** (modern, recommended) and **Autotools** (legacy) build systems.

### Building with Meson

```bash
# Configure build
meson setup build

# Build the project
meson compile -C build

# Run tests
meson test -C build

# Install
meson install -C build
```

### Building with Autotools

```bash
# Generate configure script (if needed)
./autogen.sh

# Configure
./configure

# Build
make

# Install
make install
```

### Build Options

Configure with meson options (via `-D` flags):

- `-Dappindicator=<no|yes|auto|ayatana|ubuntu>`: AppIndicator support (default: yes/auto)
- `-Dwwan=<true|false>`: Enable WWAN/ModemManager support (default: true)
- `-Dselinux=<true|false>`: SELinux label support in connection editor (default: true)
- `-Dteam=<true|false>`: Team configuration editor (default: true)
- `-Dmore_asserts=<0-100>`: Assertion level for debugging (default: 0)
- `-Dld_gc=<true|false>`: Linker garbage collection (default: true)

Example:
```bash
meson setup build -Dwwan=false -Dselinux=false
```

## Code Architecture

### Component Structure

```
src/
├── applet.c                    # Main applet application (GApplication)
├── main.c                      # Entry point for nm-applet
├── applet-device-*.c           # Device-specific handlers (WiFi, Ethernet, BT, Broadband)
├── applet-agent.c              # Secret agent for credential storage
├── connection-editor/          # Connection editor application
│   ├── main.c                  # Entry point for nm-connection-editor
│   ├── nm-connection-editor.c  # Editor window and logic
│   ├── nm-connection-list.c    # Connection list UI
│   ├── ce-page*.c              # Configuration pages (25 different page types)
│   ├── ce-polkit*.c            # PolicyKit integration
│   └── *.ui                    # GTK UI definition files (GtkBuilder)
├── wireless-security/          # EAP method implementations
├── utils/                      # Utility functions and helper libraries
└── shared/                     # Shared code between components
```

### Key Architecture Patterns

**Device Handler Pattern**: Each network device type (WiFi, Ethernet, Bluetooth, Broadband) has a dedicated handler implementing the `NMADeviceClass` interface. Handlers are registered in `applet.c` and manage device-specific UI, menus, and state.

**Page-Based Editor**: Connection editor uses a page-based architecture where each connection type/protocol has dedicated page implementations (`page-*.c`). Each page inherits from `CEPage` base class and handles UI, validation, and settings for specific connection aspects (IP, WiFi, VPN, Bridge, etc.).

**Secret Agent**: `AppletAgent` implements NetworkManager's secret agent interface, handling credential requests and storage via libsecret.

**PolicyKit Integration**: Connection editor uses `ce-polkit.c` to check permissions and enable/disable UI widgets based on NMClient permissions. PolicyKit button (`ce-polkit-button.c`) provides authentication UI.

**GtkBuilder UI**: UI definitions are in `.ui` files compiled as GResources. Access widgets via `gtk_builder_get_object()`.

### Important Dependencies

- **libnm** (>= 1.7): NetworkManager client library
- **libnma** (>= 1.8.27): NetworkManager applet library for common widgets
- **gtk+-3.0** (>= 3.10): GTK3 UI toolkit
- **libsecret-1** (>= 0.18): Secret storage
- **libnotify** (>= 0.4.3): Desktop notifications
- **libxsettings-client**: X settings integration
- **mm-glib**: ModemManager integration (optional, for WWAN)
- **jansson** (>= 2.7): JSON parsing for team config (optional)
- **libselinux**: SELinux support (optional)

### AppIndicator Support

The applet supports both traditional XEmbed system tray and AppIndicator (Ubuntu/Ayatana). Build-time selection via `-Dappindicator` option. Code paths are conditionally compiled with `#ifdef WITH_APPINDICATOR`.

## Coding Standards

From CONTRIBUTING file:

1. **Indentation**: 5-space REAL tabs (not 8-space)
2. **Brace style**: Opening brace on next line
   ```c
   if (condition)
   {
        ...
   }
   ```
3. **Line width**: Not limited to 80 characters
4. **Platform independence**: Avoid platform-specific hardcoded paths; use configurable paths

## Common Development Tasks

### Running Tests

```bash
# Run all tests
meson test -C build

# Run specific test
meson test -C build test-utils
```

Currently only one test exists: `test-utils` in `src/utils/tests/`.

### Modifying UI Files

UI files (`.ui`) are GTK GtkBuilder XML files. Edit them with Glade or text editor, then rebuild:

```bash
meson compile -C build
```

They are compiled into GResources automatically.

### Adding a New Connection Page

1. Create `page-<type>.c` and `page-<type>.h` in `src/connection-editor/`
2. Create corresponding `.ui` file for the page layout
3. Add to `sources` and `resource_data` in `src/connection-editor/meson.build`
4. Implement `CEPage` interface with required virtual functions
5. Register page in connection type mapping

### Debugging

Enable debug output:
```bash
# Run applet with debug output
nm-applet --shell-debug

# Run with GLib debug messages
G_MESSAGES_DEBUG=all nm-applet
```

Set `more_asserts` build option for additional runtime checks:
```bash
meson setup build -Dmore_asserts=100
```

### Working with PolicyKit

PolicyKit permissions are checked via `NMClient` API. To make UI elements permission-aware:

```c
ce_polkit_connect_widget(widget, tooltip, auth_tooltip, client, permission);
```

This automatically grays out/enables widgets and shows authentication prompts.

## Git Workflow

Current branch: `alse_1.8`
Main branch: `alse_1.8`

Recent activity includes PolicyKit error messages, toggle button checks, and bridge/DSL page modifications.

## File Locations (Installation)

Configured via meson options `prefix`, `bindir`, `datadir`, etc. Defaults:

- Binaries: `${prefix}/bin/` (`nm-applet`, `nm-connection-editor`)
- Desktop files: `${datadir}/applications/`
- Autostart: `${sysconfdir}/xdg/autostart/`
- Icons: `${datadir}/icons/`
- Locale: `${localedir}/`
- AppData: `${datadir}/metainfo/`

## Internationalization

Uses gettext. Translation files in `po/` directory. Domain: `nm-applet`

To update translations:
```bash
cd po
# Update .pot template
# Update .po files
```

## Additional Notes

- This is a modified fork with custom patches (indicated by version 1.30.0 vs upstream 1.20.0)
- Both `build/` and `obj-x86_64-linux-gnu/` directories exist (meson and autotools builds)
- The applet integrates with GNOME desktop session management
- Secret storage uses libsecret (GNOME Keyring backend)
- VPN plugins are loaded dynamically via libnma VPN plugin infrastructure
