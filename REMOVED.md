# Removed: telemetry, phone-home and dead weight

This fork of [videolan/vlc-android](https://github.com/videolan/vlc-android)
(base: upstream `master` @ `c3b20bc9a1070e3e8f43aee284d2a2b6b621296b`,
v3.7.2 Beta 1 era) removes every piece of code that communicated over the
network without an explicit user action, plus the dead modules that supported
it. This document is the complete inventory.

## 1. Nightly auto-updater — removed

**What it did.** On every launch of a debug/nightly build, `AutoUpdate`
silently polled `http://artifacts.videolan.org/vlc-android/nightly-<abi>/`
(at most every 6 hours), and on a hit offered to download an APK **over plain
HTTP** and side-load it. A manual "Install nightly build" entry in advanced
settings used the same code path.

**Removed.**
- `application/vlc-android/src/org/videolan/vlc/util/AutoUpdate.kt` (OkHttp poll + APK download/install)
- `application/vlc-android/src/org/videolan/vlc/gui/dialogs/UpdateDialog.kt` + `res/layout/dialog_update.xml`
- Auto-check on startup in `MainActivity` and `MainTvActivity`
- "Install nightly build" and "Check for updates" preferences (`preferences_adv.xml`, both mobile and TV `PreferencesAdvanced`)
- The `app_update` path in the app `FileProvider`, `getUpdateUri()`, and the `show_update` / `last_update_time` settings keys

## 2. Moviepedia online metadata scraper — removed

**What it did.** A dedicated Gradle module (`:application:moviepedia`) with a
Retrofit/OkHttp client for VideoLAN's media-scraping API. It sent media
titles/filenames plus `Client`, `Client-Version` and `Client-Type` headers.
An indexing hook subscribed to the end of **every media library scan** and
scraped un-indexed videos automatically (active on debug TV builds; the
endpoint URL is a build property, `https://localhost/` by default). "Find
metadata" context actions and a large part of the Android TV UI (movies /
TV-shows rows and browsers, scraped search results, next-episode logic) were
driven by its database.

**Removed.**
- The whole `application/moviepedia` module (API client, Room database, providers, viewmodels, UI)
- Auto-index hook: `ACTION_CONTENT_INDEXING` broadcast after scans (`MediaParsingService`) and the receivers in `application/app` (`IIndexersDelegate`, `IMediaContentDelegate`)
- "Find metadata" context action (`CTX_FIND_METADATA`) in the video grid and file browser
- TV: `MediaScrapingTv*` activities/fragments/adapters, `MediaScrapingBrowserTvFragment`/`ViewModel`, `MediaImageCardPresenter`, `MetadataCardPresenter`, `PersonCardPresenter`, `TvShowDescriptionPresenter`, `VideoDetailsPresenter`, scraped "Recently played"/"Recently added" home rows, moviepedia results in the TV search provider, movie/TV-show headers in the TV video row
- `MOVIEPEDIA_*`, `ACTION_OPEN_CONTENT`, `CONTENT_*` constants and the TV-content deep-link branch in `MediaUtils`

The TV interface now relies purely on the local media library.

## 3. Donations / Google Play billing — removed

**What it did.** `:application:donations` bundled `VLCBilling` and legacy
in-app-billing helpers that bind to the Play Store billing service to fetch
donation SKUs. All call sites were already commented out upstream; the module
was dead weight, and the donate buttons it fed either did nothing or stayed
hidden.

**Removed.** The whole `application/donations` module, the More-screen and
About-screen donate cards (code + layout elements), the TV home "tip jar"
card, `showDonations()`, and the `ID_SPONSOR` constant.

## 4. Chromecast renderer discovery — now strictly opt-in

**What it did.** `RendererDelegate` started libvlc renderer discoverers
(mDNS/Chromecast multicast probes on the local network) automatically
whenever the device had connectivity. The "Enable casting" preference only
hid the cast button — discovery traffic kept running even when disabled.

**Changed.** Discovery now returns early unless `enable_casting` is on, and
that preference **defaults to off** (code defaults and `preferences_casting.xml`).
Until casting is explicitly enabled in Settings → Casting, the app emits no
discovery traffic; enabling it restores the full feature.

## 5. Remote access web server — removed

**What it was.** An opt-in Ktor/Netty HTTP(S) server embedded in the app
(`:application:remote-access-server` plus the bundled Vue web client) letting
a LAN browser control playback and browse/transfer media, secured by
on-device TLS and OTP pairing. It never ran unless enabled, but it shipped a
complete inbound web stack in every APK.

**Removed.** Both modules, the settings screen, the TV home card, the OTP
display activities and notification channels, the `vlc.remoteaccess.share`
deep link, the web-client build script and the npm build-time dependency.
The app no longer contains an HTTP server of any kind.

## 6. Audited and deliberately kept (user-initiated only)

| Feature | Why it stays |
|---|---|
| Playback of streams / network shares (libvlc) | Only plays URLs and shares the user opens |
| Network browsing (SMB/UPnP/NSD discovery) | Runs when the user opens the Browse → network views |
| Subtitle download (opensubtitles.com) | Only from the user-opened "Download subtitles" dialog |
| `HttpImageLoader` artwork loading | Renders artwork URLs of media the user is browsing/playing; never crawls on its own |
| Crash/feedback reporting | Composes an email via the user's own mail app after the user agrees in a dialog; nothing is auto-submitted |
| Chromecast casting | Fully functional again once `enable_casting` is turned on |

Vestigial wording: the rarely-shown feedback fallback screen still mentions
remote access in its help text; it is text only, nothing behind it runs.

Also verified: `VLCOptions` passes no phone-home libvlc flags (the keystore
options are local credential storage); the medialibrary Java API contains no
HTTP client; TV's Play-Services check for the search affordance is a local
capability query.

## 7. Verified absent in this base (nothing to remove)

Sentry or any crash-reporting SDK, Firebase/Google analytics, Matomo,
Crashlytics, and ad SDKs — audited for and not present in this upstream
revision.

## 8. Intentional leftovers

- Translated strings, drawables and colors that belonged to removed features
  are kept to avoid churn in ~60 locale files; they are inert resources.
- The `INTERNET` permission remains — it is required for user-initiated
  streaming and network browsing.

## 9. Verification

All edits were applied as exact-match replacements, and repository-wide
symbol sweeps for every removed identifier come back clean. After the
network policy was opened, `:application:app:compileDebugKotlin` was run in
this environment and **builds successfully** — every module's Kotlin/Java
compiles against the vendored source trees, covering all removals above.
The native NDK stage has not been exercised here; run
`./buildsystem/compile.sh -l -a <abi>` locally for the full build.
