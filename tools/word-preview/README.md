# Word renderer provenance

Folderwing vendors the browser distribution of docx-preview 0.4.1 and JSZip 3.10.2.
The exact upstream package URLs, package integrity values, upstream asset hashes,
patch hash and delivered asset hashes are in `tools/word-dependencies.json`.

`docx-preview-0.4.1-folderwing.patch` adds rendering for several Office Math
structures and fraction modes. It changes the upstream distribution and is
retained to make those changes visible. The checked-in delivered file was produced
from the upstream unminified distribution using the patch and Terser 5.19.2:

```sh
patch -p1 < tools/word-preview/docx-preview-0.4.1-folderwing.patch
terser docx-preview.js --compress --mangle --comments '/@license/' -o docx-preview.min.js
# Prefix the delivered file with the Folderwing modification notice shown in the vendor file.
```

The release packager verifies the delivered JavaScript hashes. The JSZip browser
bundle declares that it contains pako. Its exact embedded transitive versions are
not independently asserted; the release retains notices for the resolved JSZip
dependency closure conservatively, including the pako and zlib source notices.
