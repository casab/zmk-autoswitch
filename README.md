# zmk-autoswitch

A [ZMK](https://zmk.dev) module that lets the [AutoSwitch KVM](https://autoswitchkvm.com) app
change your keyboard's Bluetooth profile. When you press the KVM button, AutoSwitch on the
computer you're leaving tells the keyboard to switch to the other computer's profile, the same
way it does for Logitech Easy-Switch keyboards. Your pairings stay as they are.

The keyboard speaks the same protocol as those Logitech keyboards: a small part of HID++ 2.0
(Change Host and Hosts Info), under the keyboard's own USB vendor and product ID.

> **Status:** 2.0 (HID++) is in testing. 1.x (its own protocol over zzeneg/zmk-raw-hid) worked in
> both directions between Windows and macOS on a nice!nano Reviung41.

## How it works

- Each computer pairs with its own profile, as usual (`&bt BT_SEL 0`, `BT_SEL 1`, ...).
- The module adds a second, vendor-defined HID interface over Bluetooth and USB. AutoSwitch uses
  it to read the profiles and to select one.
- When AutoSwitch first sees the keyboard while the KVM is on its computer, it marks that profile
  as "AutoSwitch runs here" and, if the profile has no name yet, stores the computer's name on it.
  Both are stored on the keyboard with the profile's pairing and are dropped if that profile is
  later paired with another computer.
- When the KVM leaves, AutoSwitch sends the keyboard to the other computer's marked profile (or the
  profile you chose in the device's settings). If no other profile is marked yet and exactly one
  other profile is paired, it uses that one. It never sends it to an empty profile.

## Add it to your zmk-config

1. `config/west.yml`: add this module.

   ```yaml
   manifest:
     remotes:
       - name: zmkfirmware
         url-base: https://github.com/zmkfirmware
       - name: casab
         url-base: https://github.com/casab
     projects:
       - name: zmk
         remote: zmkfirmware
         revision: main
         import: app/west.yml
       - name: zmk-autoswitch
         remote: casab
         revision: main
     self:
       path: config
   ```

2. `config/<keyboard>.conf` (for a split keyboard, the central half's):

   ```
   CONFIG_ZMK_AUTOSWITCH=y
   ```

3. Build and flash as usual.

4. **After flashing, remove the keyboard from each computer's Bluetooth settings and pair it
   again** (each on its own profile). The keyboard now has a second HID service, and computers
   cache the old service list.

Then open AutoSwitch ▸ Devices and add the keyboard (or reopen its settings): it should say
"Switches channel on request" with the number of profiles.

Upgrading from 1.x: remove `zmk-raw-hid` from `west.yml` and the `raw_hid_adapter` shield from
`build.yaml` (unless something else uses them), then re-pair as in step 4. The marks carry over.

## Options

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_ZMK_AUTOSWITCH` | n | Enable the module. |
| `CONFIG_ZMK_AUTOSWITCH_SELECT_DELAY_MS` | 100 | Time between replying to a profile switch and doing it, so the reply reaches the computer first. |
| `CONFIG_ZMK_AUTOSWITCH_USB` | y (with USB) | Also offer the interface over USB. Sets `CONFIG_USB_HID_DEVICE_COUNT` to 2. |

## Protocol

A HID++ 2.0 device at device index `0xFF` (a device connected straight to the computer), on a
vendor collection laid out like a Logitech Bluetooth LE device: usage page `0xFF43`, usage
`0x0202`, long report `0x11` (19 bytes after the ID) in both directions. Requests:
`0x11, 0xFF, feature index, function << 4 | software ID, parameters…`; replies echo the first four
bytes; errors are `0x11, 0xFF, 0xFF, feature index, function | software ID, error code` (2 invalid
argument, 3 out of range, 6 invalid feature index, 7 invalid function).

| Index | Feature | Functions |
|---|---|---|
| 0 | `0x0000` Root | `getFeature(id)` → index, type, version (index 0: absent); `getProtocolVersion` → 4.2 |
| 1 | `0x0001` Feature set | `getCount`, `getFeatureId(index)` |
| 2 | `0x0005` Device name | `getNameLength`, `getName(offset)`, `getType` → keyboard |
| 3 | `0x1814` Change Host (v1) | `getHostInfo` → profile count, current profile; `setCurrentHost(n)` (replies, then switches `CONFIG_ZMK_AUTOSWITCH_SELECT_DELAY_MS` later); `getCookies`; `setCookie(n, value)` |
| 4 | `0x1815` Hosts Info | `getFeatureInfo` → caps 0x03 (names readable and writable), profile count, current; `getHostInfo(n)` → n, status (1 paired), 0, 0, name length, max 32; `getHostFriendlyName(n, offset)` → n, offset, up to 14 bytes; `setHostFriendlyName(n, offset, bytes)` |

Hosts are Bluetooth profiles; `0xFF` means the current one in Hosts Info. Cookies and names are
stored with the profile's bonded address, read as empty once the profile is bonded to a different
computer, and can't be written for an unpaired profile. AutoSwitch marks its own profile with
cookie `0xA5`.

The host side is in the AutoSwitch KVM repository:
[SPECIFICATION.md §5a](https://github.com/casab/AutoSwitchKVM/blob/main/SPECIFICATION.md).

## Building it locally

`ci/build.sh` builds a test keyboard (nice!nano + Reviung41) with the module, in ZMK's build
image. CI runs it on every push (`.github/workflows/build.yml`).

```sh
docker run --rm -v zmk-ws:/ws -v "$PWD:/repo" -e WS=/ws zmkfirmware/zmk-build-arm:stable \
  bash /repo/ci/build.sh
```

The firmware lands in `out/` (git-ignored). Set `CONFIG=/path/to/your/zmk-config/config` and
`SHIELDS=...` to build your own keyboard instead.

## Notes

- Works over Bluetooth and over USB. AutoSwitch only uses it for Bluetooth keyboards.
- It isn't a Logitech device and doesn't say it is: it keeps the keyboard's own vendor and
  product ID, so Logitech's software and Linux's Logitech HID++ driver (which binds by Logitech
  product ID) leave it alone.

## License

MIT, see [LICENSE](LICENSE).
