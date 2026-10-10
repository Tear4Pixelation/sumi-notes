# iOS / iPad build

The IPA is built on GitHub's macOS runners by `.github/workflows/ios.yml`, started by hand (Actions tab >
"iOS build" > "Run workflow", or `gh workflow run ios.yml`). No workflow here runs on push - every build is
manual, so a private repository does not spend its macOS minutes (billed at 10x) on every commit.

It builds this repository at the commit it is started on: SDL from the `write-mac` branch, then
`make IOS=1 DIST=adhoc DEBUG=0 ipa` in `syncscribble/`. The app is universal (iPhone and iPad,
`UIDeviceFamily` 1 and 2), minimum iOS 15 (`IOS_MIN_VERSION`).

- **Without secrets** it uploads `sumi-ios-unsigned-ipa`: proof that the build works, or input for a
  sideloading tool that signs with a free Apple ID (AltStore, SideStore, Sideloadly).
- **With the three secrets** it uploads `sumi-ios-adhoc-ipa`, signed for the devices in the profile.

Team id and bundle id are read from the profile, so the Makefile's `IOS_BUNDLE_ID` (`de.gipflig.sumi`)
only matters for local builds - but the profile must be for that id, or the app gets a different one.

## One-time setup, from Linux

Needs the paid Apple Developer membership (ad hoc distribution) and `libimobiledevice`
(`pacman -S libimobiledevice ideviceinstaller`).

1. **Register the iPad.** Plug it in, trust the computer, `idevice_id -l` prints its UDID. Add it under
   developer.apple.com > Certificates, IDs & Profiles > Devices.
2. **App ID.** Identifiers > + > App IDs > App, explicit Bundle ID `de.gipflig.sumi`. No extra capabilities.
3. **Distribution certificate.** Make the key and request locally, upload the request, convert the result:
   ```
   openssl genrsa -out sumi-dist.key 2048
   openssl req -new -key sumi-dist.key -out sumi-dist.csr -subj "/emailAddress=YOU@EXAMPLE.COM/CN=YOUR NAME/C=DE"
   # Certificates > + > Apple Distribution, upload sumi-dist.csr, download distribution.cer
   openssl x509 -inform DER -in distribution.cer -out distribution.pem
   openssl pkcs12 -export -legacy -inkey sumi-dist.key -in distribution.pem -out sumi-dist.p12
   ```
   `-legacy` matters: macOS `security import` cannot read the AES-encrypted .p12 OpenSSL 3 writes by default.
   Keep `sumi-dist.key` safe and out of the repository; the certificate lasts a year.
4. **Profile.** Profiles > + > Ad Hoc > the App ID > the certificate > the devices. Download the
   `.mobileprovision`. Adding a device later means regenerating it and updating the secret.
5. **Secrets** (repository Settings > Secrets and variables > Actions, or `gh`):
   ```
   base64 -w0 sumi-dist.p12 | gh secret set IOS_CERTIFICATE_P12_BASE64
   gh secret set IOS_CERTIFICATE_P12_PASSWORD          # prompts for the .p12 password
   base64 -w0 Sumi_AdHoc.mobileprovision | gh secret set IOS_PROFILE_BASE64
   ```

## Building and installing

```
gh workflow run -R Tear4Pixelation/sumi-notes ios.yml && gh run watch -R Tear4Pixelation/sumi-notes
gh run download -R Tear4Pixelation/sumi-notes --name sumi-ios-adhoc-ipa   # Sumi.ipa
ideviceinstaller install Sumi.ipa                                         # older ideviceinstaller: -i Sumi.ipa
```

`-R` is needed because `gh` in this checkout resolves to upstream `styluslabs/Write` (404 on `ios.yml`);
`gh repo set-default Tear4Pixelation/sumi-notes` makes it unnecessary. The build log prints which app id it signed and when the profile expires.

## Crash logs and symbols

The IPA's binary is stripped. `Makefile.ios` runs `dsymutil` before `strip` (release builds only) and
the workflow uploads `Release/Sumi.app.dSYM` as `sumi-ios-dsym-<build>`. The build number is
`git rev-list --count HEAD`; a crash report shows it as `build_version` and as the last part of
`app_version` (3.1.121 = build 121 = the commit with 121 commits).

Pull reports from the iPad with `idevicecrashreport -e <dir>` (`Sumi-*.ips` crashes, `Sumi.cpu_resource-*`,
`JetsamEvent-*` for memory kills). Symbolicate on Linux without Xcode: the `.ips` gives the `Sumi`
image's base and each frame's `imageOffset`; the binary's `__TEXT` starts at vmaddr 0x100000000, so

```
llvm-dwarfdump --uuid Sumi.app.dSYM/Contents/Resources/DWARF/Sumi     # must match the report's slice_uuid
echo 0x100157c34 | llvm-symbolizer --obj=Sumi.app.dSYM/Contents/Resources/DWARF/Sumi --inlining
```

(subtract 4 from every frame but the top one to get the call line, not the line after it).

**Builds are not reproducible**: two CI runs of the same commit gave different UUIDs, so a dSYM only
matches the exact run that made the installed IPA - keep the artifact of every build you install.
For a build without a dSYM (anything before this change), rebuild its commit plus only the dSYM change
on a branch (as `crash-symbols-121` did), then compare `__TEXT,__text` of the old and new IPA's binary
(`llvm-objcopy --dump-section=__TEXT,__text=out.bin`): if the bytes are identical the offsets are exact
even though the UUID differs; the extra commit only changes the embedded short hash
(`SCRIBBLE_REV_NUMBER`) and the build number in the Info.plist, both the same length.

## Resume (black screen after returning from background)

What SDL's UIKit backend sends (`SDL/src/video/SDL_video.c`, `SDL_OnApplication*`):

- resign active: `FOCUS_LOST`, `MINIMIZED`, then `SDL_APP_WILLENTERBACKGROUND` (also for Control Center or
  an app switcher peek that never reaches the background)
- `SDL_APP_DIDENTERBACKGROUND`, `SDL_APP_WILLENTERFOREGROUND`
- become active: `SDL_APP_DIDENTERFOREGROUND`, then per window `FOCUS_GAINED` and `RESTORED`
- never `EXPOSED`. Resizes come from `viewDidLayoutSubviews` as `RESIZED`/`SIZE_CHANGED`, and SDL drops
  any still-queued size event when a new one arrives (`RemovePendingSizeChangedAndResizedEvents`).

How the app handles them: the filter (`ScribbleApp::sdlEventFilter`, runs inside the UIKit callback on the
main thread) saves on WILLENTERBACKGROUND, sets `Application::isSuspended` on DIDENTERBACKGROUND and clears
it on DIDENTERFOREGROUND. While suspended `tracedGuiLayoutAndDraw()` returns before `SvgGui::layoutAndDraw`,
so no GL calls and the dirty state is kept, not lost. All off-screen rendering (thumbnails, PDF, screenshot)
is `PAINT_SW`; GL is only touched inside the frame. The GL context is never destroyed, so textures and
`nvglFB` survive. Every frame blits the whole of `nvglFB` to SDL's renderbuffer, so *any* frame repaints
the full screen.

**Fixed bug (confirmed from code):** nothing forced a frame on resume. `SvgGui::sdlWindowEvent` repaints
everything on `EXPOSED`, and on `RESTORED` only under `#if PLATFORM_ANDROID`; iOS sends no `EXPOSED`. So
after resume nothing was dirty and no frame was presented until the user touched something. If the
CAEAGLLayer still held its last frame that is invisible; if not (see below) the screen is black.
`ScribbleApp::sdlEventHandler` now unions the window into `gui->closedWindowBounds` on `RESTORED` under
`PLATFORM_IOS`, exactly what the `EXPOSED` case does. (The cleaner fix is `PLATFORM_ANDROID` ->
`PLATFORM_MOBILE` in ugui's `sdlWindowEvent`; done in the app to avoid a ugui fork commit.)

Why the layer can be empty on resume (hypotheses, not verified on a device):

- iOS may discard a backgrounded app's layer backing store under memory pressure. `SDL_GL_RETAINED_BACKING`
  is 0, so presented content is not guaranteed to persist. Fits "sometimes".
- iPadOS takes app switcher snapshots in the other orientation after `DIDENTERBACKGROUND`. That runs
  `SDL_uikitopenglview layoutSubviews` -> `updateFrame`, which reallocates the renderbuffer
  (`renderbufferStorage:fromDrawable:`, contents undefined) - SDL issues those GL calls in the background
  itself, outside our gate. On the way back the two size events can coalesce into one with the original
  size, so the app may see no size change at all and redraw nothing. The fix above covers this too, since
  `RESTORED` comes after any snapshot pass.

Untested: none of this has run on an iPad. To verify, lock/unlock and use the app switcher (including
rotating the iPad while Sumi is in the switcher) with a document open and with the document list open.

## Gaps

- No TestFlight / App Store upload: that needs an App Store profile and an upload step with an App Store
  Connect API key. The Makefile already builds the same IPA for any distribution profile (`DIST` != 0).
- The job first ran on 2026-10-03 (unsigned IPA); the first ad hoc signed IPA came from the 2026-10-04 run.
  Whether that IPA installs and runs on the iPad is not recorded here.
