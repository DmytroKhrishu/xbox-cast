# Building Airtime

Airtime (repository and project files: xbox-cast / `XboxCast`) is a C++/WinRT UWP app (`app/XboxCast.vcxproj`, x64
only) for Xbox: sideloaded in Dev Mode, or installed from the Microsoft Store. The receiver core
(`native/`, castcore + vendored UxPlay) is a static C library built for `x64-uwp` by `native/build-uwp.ps1`. No build ever
contains an address, host name or personal value: identities and keys come from your CI settings.

## Build on GitHub Actions (a fork, no Windows machine needed)
`.github/workflows/build.yml` runs on every push to `app/**` or `native/**` on `main`, or by hand (Actions → build → Run
workflow). On `windows-2025-vs2026` it:
1. builds castcore with `pwsh native/build-uwp.ps1 -Arch x64 -Config Release` when that script exists (vcpkg binary cache
   in `actions/cache`); otherwise the app builds without the receiver (`XC_HAVE_CASTCORE` off: idle screen only);
2. signs with your certificate (below) and checks its subject against the manifest `Publisher`;
3. stamps the version `0.1.<run_number + VERSION_OFFSET>.0` and the sideload identity into `Package.appxmanifest`, and
   the display name `Airtime (dev)` into `Resources.resw` (so it is told apart from the Store build on one console);
4. builds the package (`/p:UapAppxPackageBuildMode=SideloadOnly /p:AppxBundle=Never`);
5. smoke-checks it: unzips the `.msix`, lists every file, requires `XboxCast.exe`, `resources.pri` and the FFmpeg DLLs, and
   fails if any packaged file contains a LAN IP (`192.168.x.x`) or a tailnet name;
6. uploads the artifact **`xboxcast-x64`** (kept 3 days): the `.msix` plus `Dependencies/x64/*.appx` (no `*.Desktop.*`).

The build log prints `xbox-cast: XC_HAVE_CASTCORE=true|false ...` (MSBuild) and the smoke step prints the package identity
and version; the run summary repeats them.

Repository **secrets** (Settings → Secrets and variables → Actions → Secrets):

| Secret | Needed | What |
|---|---|---|
| `SIGNING_PFX_B64` | recommended | base64 of a code-signing `.pfx` whose subject is `CN=XboxCastDev`. Without it each run signs with a throwaway cert (a `::warning::` says so): it installs, but can't update the previous build in place (`deploy.py --reinstall`). |
| `SIGNING_PFX_PASSWORD` | with the pfx | its password |

Repository **variables** (optional):

| Variable | What |
|---|---|
| `SIDELOAD_IDENTITY` | package Identity Name, default `XboxCast.Sideload` (from `Package.appxmanifest`). 3-50 of `A-Z a-z 0-9 . -`. Set it to the Name of an existing install to keep upgrading it in place. |
| `SIDELOAD_PHONE_PRODUCT_ID` | overrides `mp:PhoneIdentity PhoneProductId` (a GUID). Only needed together with `SIDELOAD_IDENTITY` when an existing install used a different one. |
| `VERSION_OFFSET` | added to the CI run number in the version. Set it when you move to a new repo so new builds still install over older ones. |

The product name **Airtime** lives only in `app/Assets/Strings/en-US/Resources.resw` (`AppDisplayName`, used by the
manifest as `ms-resource:AppDisplayName` and by the About page). The AirPlay name the iPhone shows under Screen Mirroring
is the user setting `deviceName` (default `Xbox`), not the product name.

## Microsoft Store build
`.github/workflows/build-store.yml`, manual only (Actions → build-store → Run workflow, or
`gh workflow run build-store.yml`). It builds castcore (required here), stamps the **Store identity** from repository
variables and the version `1.0.<run_number + VERSION_OFFSET>.0`, and builds twice with `/p:XcStoreBuild=true`
(`XC_STORE_BUILD`: the dev server setting defaults to off):

| Artifact (kept 3 days) | What |
|---|---|
| `airtime-store-upload` | `Airtime…_x64.msixupload` (`StoreUpload`, bundle, unsigned). Upload this to Partner Center; Microsoft signs it. |
| `airtime-store-test-x64` | the same identity as a sideloadable `.msix` signed with the Store test cert, plus `Dependencies/x64`. |

The smoke step unpacks both packages (the upload via its bundle), requires `XboxCast.exe`, `resources.pri` and the FFmpeg
DLLs, castcore's version string in the exe, the Store Name/Publisher/Version in `AppxManifest.xml`, and no LAN IP or
tailnet name in any file. The Store version line is independent of the sideload `0.1.x` line (different identity), but
every upload must be higher than the last one Partner Center accepted: never lower `VERSION_OFFSET`.

The manifest keeps `TargetDeviceFamily Windows.Universal` (MaxVersionTested 10.0.26100.0); restrict it to Xbox in Partner
Center (Packages → Device family availability: tick Xbox only).

Repository **variables** (values from Partner Center → the app → Product management → Product identity; never commit
them):

| Variable | What |
|---|---|
| `STORE_NAME` | Package/Identity/Name |
| `STORE_PUBLISHER` | Package/Identity/Publisher (`CN=<guid>`) |
| `STORE_PUBLISHER_DISPLAY` | Package/Properties/PublisherDisplayName |
| `STORE_PHONE_PRODUCT_ID` | any GUID for `mp:PhoneIdentity`, generated once, never changed |
| `STORE_DISPLAY` | display name, default `Airtime` (the reserved Store name) |

Repository **secrets**: `STORE_TEST_PFX_B64` / `STORE_TEST_PFX_PASSWORD`, a code-signing `.pfx` whose subject equals
`STORE_PUBLISHER` (make it like the sideload cert above, with that subject). Keep it stable: a new key means the next test
build can't update the installed one. Without it each run signs with a throwaway cert (a `::warning::` says so).

Check the Store build on a Dev Mode console next to the dev build before uploading:

```sh
tools/deploy.py --run-id <id> --artifact airtime-store-test-x64 --launch
```

## Art
Every PNG in `app/Assets` and the Store listing art in `docs/store/art/` come from one generator (Node +
`@resvg/resvg-js`, nothing installed in the repo). The mark lives in `docs/store/art/src/marks.mjs`; listing title text
uses Inter Display SemiBold (SIL OFL, not committed).

```sh
mkdir -p "$TMPDIR/art" && cd "$TMPDIR/art" && npm init -y >/dev/null && npm install @resvg/resvg-js
curl -sSLO https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip && unzip -q Inter-4.1.zip -d inter
R=<repo>/docs/store/art/src
node $R/build.mjs        # app/Assets/*.png, each at its existing size and name
node $R/store.mjs        # app tile icon, box art, poster (title in the top 2/3), super hero (no text), mark.svg
node $R/xbox-extras.mjs  # Xbox branded key art, titled hero, featured square
node $R/concepts.mjs     # docs/store/art/concepts.png review sheet (run last)
```

To add a package image, create a PNG with the wanted name and size in `app/Assets` and rerun `build.mjs`.

### Capabilities
`internetClient`, `internetClientServer`, `privateNetworkClientServer` (AirPlay listens on the LAN), and the restricted
capability **`hevcPlayback`** (`rescap:`). On Xbox it unlocks HEVC hardware decode, the larger memory budget and 4K
output; in exchange the app is suspended when a game launches and there is no background audio (owner decision,
`docs/decisions.md`). Sideloading needs nothing extra. A Store submission needs a one-time justification for it in Partner
Center (e.g. "HEVC hardware decoding of mirrored 4K screen content from iOS devices").

### Signing certificate
The certificate's subject must equal the manifest's `Publisher` (`CN=XboxCastDev`; the workflow checks it). Make one
once and keep the `.pfx` backed up: a new key means no in-place updates (uninstall first).

With OpenSSL (Linux/macOS):

```sh
openssl req -x509 -newkey rsa:3072 -sha256 -days 3650 -nodes -subj "/CN=XboxCastDev" \
  -addext "keyUsage=critical,digitalSignature" -addext "extendedKeyUsage=codeSigning" \
  -addext "basicConstraints=critical,CA:FALSE" -keyout xboxcast.key -out xboxcast.crt
openssl pkcs12 -export -inkey xboxcast.key -in xboxcast.crt -out XboxCast.pfx -name "xbox-cast sideload"
base64 -w0 XboxCast.pfx    # paste as SIGNING_PFX_B64 (macOS: base64 -i XboxCast.pfx)
rm xboxcast.key            # the .pfx holds the key now; keep it somewhere safe
```

If Windows refuses to import the `.pfx` (old Windows builds don't read the OpenSSL 3 default encryption), export it again
with `-certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1`.

With PowerShell (Windows):

```powershell
$c = New-SelfSignedCertificate -Type Custom -Subject "CN=XboxCastDev" -KeyUsage DigitalSignature `
  -FriendlyName "xbox-cast sideload" -CertStoreLocation Cert:\CurrentUser\My `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}") -NotAfter (Get-Date).AddYears(10)
$pw = Read-Host -AsSecureString "pfx password"
Export-PfxCertificate -Cert $c -FilePath XboxCast.pfx -Password $pw
[Convert]::ToBase64String([IO.File]::ReadAllBytes("XboxCast.pfx")) | Set-Clipboard   # paste as SIGNING_PFX_B64
```

If you change `Publisher` in the manifest, make the certificate with the same subject.

## Build locally (Windows)
Visual Studio 2026 (or 2022) with the workloads **Universal Windows Platform development** and **Desktop development with
C++**, plus the component **C++ (v143/v145) Universal Windows Platform tools** and Windows SDK 10.0.26100. The project
uses the default toolset of your Visual Studio (v145 on 2026, which is what CI uses; v143 on 2022).

```powershell
# optional: the receiver core (needs vcpkg, see native/README or the core workflow); without it the app shows its idle
# screen only
pwsh native/build-uwp.ps1 -Arch x64 -Config Release

nuget restore app\XboxCast.sln
msbuild app\XboxCast.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxBundle=Never `
  /p:UapAppxPackageBuildMode=SideloadOnly /p:AppxPackageDir=$PWD\out\ `
  /p:PackageCertificateKeyFile=$PWD\XboxCast.pfx /p:PackageCertificatePassword=<pfx-password>
```

Or open `app\XboxCast.sln`, pick `Release | x64`, and deploy to the console directly (Debug → Remote Machine,
`<xbox-lan-ip>`; Visual Studio pairs with Dev Home) or Project → Publish → Create App Packages → Sideloading.

### Project conventions (for code under `app/src/`)
- Files are picked up by wildcard: `src\**\*.cpp` (ClCompile, every file starts with `#include "pch.h"`), `*.h`, `*.xaml`
  (Page; its code-behind is the same name `.h/.cpp/.idl` next to it), `*.idl` (Midl), shaders. No project edit needed.
- Include paths: `app/`, `app/src/` (so `#include "core/ReceiverHost.h"`), the generated files dir, castcore's
  `native/out/uwp-x64/include`, FFmpeg's `include/` (`#include <libavcodec/avcodec.h>`, inside `extern "C"`).
- Shaders: name them `NAME_vs.hlsl` (vertex), `NAME_ps.hlsl` (pixel) or `NAME_cs.hlsl` (compute), entry point `main`,
  shader model 5.0. Each compiles to the header `shaders/NAME_xx.h` in the generated files dir with
  `const BYTE g_NAME_xx[]`; use it as `#include "shaders/video_ps.h"` → `g_video_ps`, `sizeof(g_video_ps)`. `*.hlsli` are
  shared includes. Shaders are compiled at build time; nothing is compiled or loaded from disk at runtime.
- Linked for everyone: `d3d11 dxgi d3dcompiler xaudio2 ws2_32`, FFmpeg (`avcodec avformat avutil swresample swscale`; the
  NuGet package packages its DLLs next to the exe), and every lib in castcore's `link.txt` when it exists
  (`XC_HAVE_CASTCORE` is then defined).
- `XC_STORE_BUILD` is defined for Store builds (`/p:XcStoreBuild=true`, build-store.yml): the `devServer` setting then
  defaults to off.

## Deploy to an Xbox in Dev Mode
1. Activate Developer Mode on the console (Microsoft's Dev Mode Activation app) and restart into Dev Home.
2. In Dev Home enable **Remote Access** (Device Portal) with a user name and password. The portal is at
   `https://<xbox-lan-ip>:11443`.
3. On the dev machine: `cp .env.example .env` and fill in `XBOX_HOST`; put `user:pass` into the file named by
   `XBOX_PORTAL_CRED_FILE` (default `~/.config/xbox-devportal.cred`, `chmod 600`).
4. Install the latest CI build (needs `gh` signed in to an account that can read the repo's artifacts):

```sh
tools/deploy.py                      # latest run's xboxcast-x64 → Device Portal install (+ dependencies)
tools/deploy.py --run-id <id> --launch
tools/deploy.py --reinstall          # signing cert changed: uninstall the old package first
tools/deploy.py --skip-install       # only download and list the package (.deploy/), don't touch the console
tools/deploy.py --artifact airtime-store-test-x64 --run-id <id>   # the Store-identity test build (side by side)
```

`--reinstall`, `--launch` and the final report act on packages whose name contains the downloaded package's Identity
Name (override with `--match`), so the dev and Store builds never touch each other.

By hand: Device Portal → **Home** → **Add**: pick the `.msix`, then add the packages from `Dependencies/x64/` (skip
`*.Desktop.*`), Start.

## Diagnostics on the console
The app runs a dev HTTP server on TCP 7099 (setting `devServer`, on by default in sideload builds; LAN peers only):

```sh
tools/devlog.py status               # version, receiver state, client, settings, media stats, memory
tools/devlog.py log -f               # follow the log ring (4000 lines)
tools/devlog.py cmd '{"viewMode":"Fill"}'
tools/devlog.py cmd '{"setting":{"showStats":true}}'
tools/devlog.py cmd '{"mdnsTest":true}'   # probe the OS DNS-SD API with a dummy _xboxcast-test._tcp service
tools/devlog.py key A Down A         # controller input, as the UI receives it
tools/devlog.py hold B --ms 1200
```

Other `/cmd` keys handled by the platform: `{"zoom":1.5}`, `{"disconnect":true}`, `{"restartReceiver":true}`,
`{"resetSettings":true}`. Every `/cmd` body is also forwarded to the UI (`docs/contract.md` §4).
