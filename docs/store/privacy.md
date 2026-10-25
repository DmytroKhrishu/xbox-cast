# Airtime privacy policy

Effective date: 25 October 2026

Airtime is a free, open-source app for Xbox that receives AirPlay screen mirroring from an iPhone or iPad on the same
local network and shows it on the TV. This policy explains what the app does with information. The short version:
**Airtime collects nothing, sends nothing to the developer or to anyone else, and has no account, no ads, no analytics
and no telemetry.**

## What Airtime does not do
- It does not collect, sell or share personal information.
- It does not contact the internet. It has no servers, no sign-in, no advertising and no analytics or crash-reporting
  services, and it includes no third-party tracking code.
- It does not record, save or upload what you mirror. The picture and sound from your device are decoded, shown on the
  TV and discarded frame by frame.
- It does not access your Microsoft account, your contacts, your files, your location, your camera or your microphone.

## What stays on your console
Airtime keeps a few things in its own app storage on the console so it works the same way next time:
- **Settings** you choose: the receiver name that your iPhone or iPad shows, resolution, frame rate, view mode, zoom,
  sound on or off, audio delay, whether a connection code is required, and whether the stats panel is shown.
- **A random receiver ID and a receiver key.** AirPlay needs a stable identity for the receiver. Airtime generates
  them on first launch; they identify the app installation, not you, and never leave the console except as part of
  the normal AirPlay connection to your own device.

Airtime does not keep a list of the devices that connected to it. While it runs, it keeps a short diagnostic log in
memory (for example "an iPhone connected", picture size, frame rate). That log may include the name and network address
your device reports. It is not written to storage and is gone when the app closes.

Uninstalling Airtime removes all of this. You can change the name and every setting at any time in the app's Settings.

## How the local network is used
Screen mirroring works only between devices on the same local network:
- Airtime announces itself on the local network (multicast DNS, also known as Bonjour) so your iPhone or iPad can list
  it under Screen Mirroring. The announcement contains the receiver name, the receiver ID and what video formats the
  receiver supports.
- When you choose it, your device connects directly to the console over the local network and sends the encrypted
  screen picture and sound. Nothing passes through any server of the developer or of a third party.
- If you turn on **Require code to connect**, Airtime shows a code on the TV that your device must enter before
  mirroring starts. Use it when other people share your network.

The app declares Windows network capabilities (including the ones for incoming connections) only so that devices on
your local network can connect to it.

Builds published in the Microsoft Store do not run any developer or diagnostics server. (Developer test builds, which
are not distributed through the Store, can enable a diagnostics service reachable only from the local network.)

## Microsoft and Apple
Airtime is distributed through the Microsoft Store and runs on Xbox. Microsoft may collect information about the Store,
the download and the console under its own terms; see the
[Microsoft Privacy Statement](https://privacy.microsoft.com/privacystatement). Your iPhone or iPad is governed by
Apple's privacy policy. The developer of Airtime receives none of that information beyond the aggregate download and
crash statistics that Partner Center shows to every Store publisher.

## Children
Airtime collects no personal information from anyone, including children.

## Open source
Airtime is free software under the GNU General Public License version 3. The complete source code, which lets anyone
check the statements above, is at https://github.com/DmytroKhrishu/xbox-cast.

## Changes
If this policy changes, the new version will be published at this address with a new effective date. Its history is
public in the repository.

## Contact
Questions about privacy or the app: open an issue at https://github.com/DmytroKhrishu/xbox-cast/issues.

AirPlay, iPhone and iPad are trademarks of Apple Inc. Xbox is a trademark of the Microsoft group of companies. Airtime
is not affiliated with or endorsed by Apple or Microsoft.
