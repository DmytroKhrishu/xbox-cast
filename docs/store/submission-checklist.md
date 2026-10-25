# Partner Center submission checklist: Airtime 1.0

First submission, **Private audience**. Do the steps in this order; texts come from the other files in this folder.
The product (name Airtime) is already reserved under the owner's publisher account. Store identity values live
only in the repo variables and Partner Center (never in this repo).

Microsoft Learn reference for every page: [Create an app submission](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/create-app-submission).

## 0. Before starting
- [ ] A Store build exists: `.msixupload` from the Store workflow (platform child), built with `XcStoreBuild=true`
      (build log line `store=true`; devServer off), version higher than any earlier upload, Identity/Publisher equal
      to Partner Center → Product identity.
- [ ] The Store-identity test package was sideloaded once and launches (idle screen, new icon, name Airtime).
- [ ] Screenshots captured per `listing.md` → Screenshots (1920×1080 PNG, no personal content on them).
- [ ] Store art ready (`docs/store/art/`): 2:3 poster, 1:1 box art, 300×300 tile icon, 16:9 super hero art (no
      text), Xbox branded key art 584×800, titled hero 1920×1080, featured square 1080×1080.
- [ ] Privacy URL opens without sign-in:
      https://github.com/DmytroKhrishu/xbox-cast/blob/main/docs/store/privacy.md
- [ ] `tools/scrub-check.sh` printed `clean` for the commit the build came from.

## 1. Pricing and availability
[Docs](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/price-and-availability)
- [ ] **Markets:** all markets (default). Nothing in the app is region-specific.
- [ ] **Visibility → Audience: Private audience.** Pick the owner's existing known user group (the one already used for
      JellyShell); reuse it, don't create a new one. Group members need a *personal* Microsoft account; work/school
      accounts can't see private apps
      ([Visibility options](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/visibility-options)).
- [ ] Leave **"Make this product public on"** unticked. Going Public later is its own submission. One-way: once any
      submission goes out as Public, the product can never go back to Private.
- [ ] Copy the private-audience link shown there (also on App identity: "URL if your app is only visible to certain
      people"). That link is how the console gets it; Store search won't list a private app on the web.
- [ ] **Discoverability:** default ("Make this product available and discoverable in the Store"); it applies only to
      the private audience.
- [ ] **Pricing:** Free. No free trial. No sales.
- [ ] Organizational licensing: untick (Store for Business/Education don't apply to Xbox).

## 2. Properties
[Docs](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/enter-app-properties)
- [ ] **Category:** Utilities + tools. **Secondary category:** Entertainment. (Reasoning: `listing.md` → Category.
      Never Games: that can't be changed later.)
- [ ] **Privacy policy:** "Does this product access, collect or transmit personal information?" Partner Center forces
      Yes because the package declares network capabilities
      ([Support info](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/support-info));
      enter the privacy URL above. (Fallback if the URL is rejected: Partner Center also accepts the policy text
      pasted directly; paste `privacy.md` without the Markdown.)
- [ ] **Website:** https://github.com/DmytroKhrishu/xbox-cast
- [ ] **Support contact info (required for Xbox):** https://github.com/DmytroKhrishu/xbox-cast/issues
      The field accepts "the URL of the web page where your customers can go for support ... or an email address"
      (same Support info page). Decided: use the owner's support email (kept out of the repo; the orchestrator enters
      it in Partner Center). The issues URL stays as the Website field.
- [ ] Phone number / address: optional for individual developers; leave empty.
- [ ] **Product declarations:** leave the default ticks (install to alternate drives, backups). Do **not** tick
      "allows users to make purchases without the Microsoft Store commerce system", "tested to meet accessibility
      guidelines" or "incorporates generative AI features"
      ([Product declarations](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/product-declarations)).
- [ ] **System requirements:** leave the hardware table empty (optional). The controller is the only input; if an
      Xbox controller / game controller row is offered, mark it under Minimum. Additional requirements text goes in the
      Store listing (step 5).
- [ ] Display mode: nothing to tick (those options are for games).

## 3. Age ratings
- [ ] Answer per `iarc.md`; expect the lowest rating everywhere with no interactive elements. Save and generate.

## 4. Packages
[Docs](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/upload-app-packages)
- [ ] Upload the `.msixupload` (not the test `.msix`). Wait for "Validated".
- [ ] **Device family availability: tick Xbox only**, untick Windows 10/11 Desktop, Holographic and the rest. A
      `Windows.Universal` package is offered to every family except Xbox by default, so this step is the one that
      makes it an Xbox app.
- [ ] Leave "Let Microsoft decide whether to make this app available to future device families" unticked.
- [ ] Save.

## 5. Store listings → English
[Docs](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/add-and-edit-store-listing-info)
From `listing.md`, field by field:
- [ ] Product name: Airtime.
- [ ] Description, Short description. "What's new": blank for 1.0.
- [ ] Product features (16).
- [ ] Screenshots: Xbox tab only, 8 shots in the listed order with their captions. No Desktop screenshots.
- [ ] Store logos: 2:3 poster, 1:1 box art, 1:1 app tile icon. Tick "Only use images I upload" if the package logos
      look worse than the uploaded ones.
- [ ] Trailers and additional assets: 16:9 super hero art; Xbox images (branded key art, titled hero, featured square).
      No trailer.
- [ ] Supplemental: Short title `Airtime`; Additional system requirements (minimum + recommended lists).
- [ ] Additional information: Keywords (7), Copyright and trademark info, Additional license terms (GPL URL),
      Developed by `DmytroKhrishu`.

## 6. Submission options
[Docs](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/manage-submission-options)
- [ ] Publishing hold: default (publish as soon as it passes certification).
- [ ] **Restricted capabilities → hevcPlayback:** paste `cert-notes.md` section 2.
- [ ] Notes for certification on this page: leave empty, the product-level page (step 7) holds them.

## 7. Additional testing information (product level, not inside the submission)
- [ ] Product → Additional testing information → paste `cert-notes.md` section 1 → Save description. Credentials table:
      empty (no sign-in).

## 8. Submit
- [ ] Re-check: Audience = Private, Device family = Xbox only, price Free, category Utilities + tools, privacy and
      support URLs filled, hevcPlayback justification present.
- [ ] Owner gives the go; **Submit to the Store**. The restricted-capability review can add time to certification.

## 9. After certification
- [ ] On the console (retail mode), signed in with a Microsoft account in the known user group: open the
      private-audience link (Edge on the console, or a page with an `ms-windows-store://pdp/?productid=<store-id>` link),
      install, launch, mirror once from an iPhone (≤ 20 s), leave the app on its idle screen.
- [ ] If the console Store shows "Something went wrong" right after the "published" mail, wait 24–48 h before opening
      a Partner Center support ticket (product ID + error code).
- [ ] Updates: product overview → Start update → Packages: upload the new `.msixupload` (higher version), Save,
      re-check Device family = Xbox only → review the testing notes → Submit. The published version stays installable
      while an update is in certification.

## Later: going Public
- [ ] Re-read `listing.md` and `privacy.md` (written for the public already), test on more consoles (original Xbox
      One untested), then a new submission with Audience = Public. It is certified again and can't be undone.
