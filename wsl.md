# Adding Ubuntu 26.04 LTS as a WSL Distro

Steps for installing the latest Ubuntu LTS (26.04, "Resolute Raccoon") on
Windows Subsystem for Linux. Run all commands in **PowerShell** (Windows
Terminal recommended).

## Prerequisites

- Windows 11, or Windows 10 version 21H2 or later (physical machine)
- Intel/AMD 64-bit or ARM 64-bit architecture
- WSL 2.4.10+ (needed for the new tar-based `.wsl` distro format)
- Virtualization enabled in BIOS/UEFI (VT-x / AMD-V)

## 1. Install or update WSL itself

If WSL is not installed yet (the command also enables the required Windows
features and may request a reboot):

```powershell
wsl --install
```

If WSL is already installed, bring it up to date:

```powershell
wsl --update
wsl --version
```

If the first `wsl --install` only enabled features, reboot and run it again.

## 2. Check available distros

List the distros available online and confirm `Ubuntu-26.04` is present:

```powershell
wsl --list --online
```

## 3. Install Ubuntu 26.04

```powershell
wsl --install Ubuntu-26.04
```

Equivalent long form:

```powershell
wsl --install -d Ubuntu-26.04
```

If the download hangs at 0.0% (Store/network issues), force a direct web
download:

```powershell
wsl --install --web-download Ubuntu-26.04
```

When installation finishes, a console window opens and asks you to create a
**UNIX username and password** for the distro. This is separate from your
Windows account.

## 4. Verify the installation

```powershell
wsl --list --verbose
```

You should see `Ubuntu-26.04` registered and running WSL 2.

## 5. Launch and use the distro

```powershell
# Enter the distro
wsl -d Ubuntu-26.04

# Make it the default distro for plain `wsl` commands
wsl --set-default Ubuntu-26.04
```

You can also launch it from the Start menu ("Ubuntu 26.04") or from the
Windows Terminal profile dropdown. First launch may take a few seconds;
subsequent launches are near-instant.

Once inside, update the system:

```bash
sudo apt update && sudo apt upgrade -y
```

## Renaming a distro (custom name)

`wsl --install Ubuntu-26.04` always registers it as `Ubuntu-26.04`. To install
under a custom name — or rename later — use export/import, which also lets you
pick where the disk lives:

```powershell
# Rename after a normal install
wsl --export Ubuntu-26.04 D:\wsl\ubuntu-2604.tar
mkdir D:\wsl\mydistro
wsl --import MyCustomName D:\wsl\mydistro D:\wsl\ubuntu-2604.tar
wsl --unregister Ubuntu-26.04
```

To install directly with a custom name, download the `.wsl` image and use
`--name` (note: `curl.exe` — bare `curl` in PowerShell is an alias for
`Invoke-WebRequest`):

```powershell
curl.exe -LO https://releases.ubuntu.com/resolute/resolute-wsl-amd64.wsl
wsl --install --from-file .\resolute-wsl-amd64.wsl --name MyCustomName
```

After either route the distro may boot as root with no default user. Fix it:

```powershell
wsl -d MyCustomName -u root
# inside: adduser myuser && usermod -aG sudo myuser
wsl --manage MyCustomName --set-default-user myuser
```

## Alternative: install from a downloaded `.wsl` image

Useful without the Microsoft Store, or for trying pre-release builds.

1. Download the WSL image from the Ubuntu archive:
   [releases.ubuntu.com/resolute](https://releases.ubuntu.com/resolute/) —
   pick the `resolute-wsl-amd64.wsl` file (or the ARM64 variant on ARM
   hardware).
2. Install it:

   ```powershell
   wsl --install --from-file .\resolute-wsl-amd64.wsl --name Ubuntu-26.04
   ```

   (Double-clicking the `.wsl` file also works.)

Note: images installed this way drop you at a `root` shell — create your user
and set it as default manually, or use the standard `wsl --install` method
above for the guided experience.

## Troubleshooting

- **Errors right after enabling features:** reboot, then re-run the install
  command.
- **Check WSL version:** `wsl --version` (need 2.4.10+ for the new format).
- **Convert an older distro to WSL 2:**
  `wsl --set-version <DistroName> 2`
- **Remove the distro later (deletes all its data):**
  `wsl --unregister Ubuntu-26.04`

## References

- [Install Ubuntu on WSL 2 — Ubuntu docs](https://ubuntu.com/wsl/docs/stable/howto/install-ubuntu-wsl2)
- [Get Ubuntu on WSL](https://ubuntu.com/download/wsl)
- [Install WSL — Microsoft Learn](https://learn.microsoft.com/en-us/windows/wsl/install)
