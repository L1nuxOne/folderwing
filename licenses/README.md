# Third-party notice sources

Folderwing's own code remains covered by the root MIT LICENSE.

`third-party/` preserves 106 notice files from the current archive, WebP, PDFium,
libvterm, mpv API-header, Zig toolchain and pinned LGPL media-build inputs. The component inventory and
hashes are in `tools/dependency-inventory.json`; status and remaining work are in
`docs/DEPENDENCIES.md`.

These are source-side notice copies prepared for future packaging. They are not
a substitute for the media runtime package and exact source bundle, a completed
runtime link-map audit, or evidence that every copied file is already installed
in the development build output. PDFium's package includes multiple component
notices; its top-level MIT packager license must not replace those notices.

Do not relicense third-party code or strip existing copyright attributions.
Preserve upstream texts; update their version/hash bindings when dependencies change.
