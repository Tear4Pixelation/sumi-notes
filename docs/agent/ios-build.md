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
gh workflow run ios.yml && gh run watch
gh run download --name sumi-ios-adhoc-ipa           # Sumi.ipa
ideviceinstaller install Sumi.ipa                   # older ideviceinstaller: -i Sumi.ipa
```

The build log prints which app id it signed and when the profile expires.

## Gaps

- No TestFlight / App Store upload: that needs an App Store profile and an upload step with an App Store
  Connect API key. The Makefile already builds the same IPA for any distribution profile (`DIST` != 0).
- As of 2026-10-03 the iOS job has never run (no Mac here, and it was not run on GitHub yet), so the
  first run may still need fixing.
