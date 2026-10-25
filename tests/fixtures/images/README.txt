Synthetic fixtures generated with Pillow 12.3.0 for OpenPilot tests. No third-party images.
alpha.png / alpha.webp: 16 x 8; left red BGRA 40,20,240,255; right blue
BGRA 240,160,20,128 in top half, alpha zero in bottom half. WebP is lossless.
oriented.jpg / oriented.tiff: same colours, EXIF orientation 6, camera/date metadata.
profile.png: RGB image with an embedded standard sRGB ICC profile.
animated.webp / animated.gif: two 12 x 6 opaque frames, red then blue.
The first-frame tests expect red; later-frame tests expect blue.
make_motion.py uses the standard library to regenerate the following fixtures:
shapes.svg: same layout and colours as alpha.png, for vector transforms/transparency.
viewbox.svg: 24 x 12 viewBox without explicit dimensions.
partial.svg: supported red rectangle plus unsupported text and an unfetched remote image.
doctype.svg / broken.svg / oversize.svg: refused XML/entities, syntax and dimension bounds.
disposal.gif: 6 x 4 canvas, four offset frames with restore-previous and clear-to-background
(disposals 3 and 2), transparent background, delays 120/70/90/110 ms, two total passes.
