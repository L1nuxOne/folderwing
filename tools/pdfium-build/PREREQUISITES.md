# Controlled PDFium candidate prerequisites

This private candidate imports Windows UCRT plus `VCRUNTIME140.dll` and
`VCRUNTIME140_1.dll`. None of those DLLs are included in this archive.

Use Windows 10/11 x64 and Microsoft's supported **Visual C++ v14 x64
Redistributable**, version **14.44.35229 or newer**. UCRT is a Windows component.
Use the official installer for the VC runtime; copying arbitrary DLL downloads
beside Folderwing is not the installation method.

- [Microsoft download and compatibility guidance](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170)
- [Official x64 installer](https://aka.ms/vc14/vc_redist.x64.exe)

The test machine already had a compatible runtime; no runtime installation or
system DLL modification was needed for acceptance.

`/MD` moves the principal CRT implementation to those external DLLs. It still
links Microsoft startup/support object code into PDFium. This candidate is not a
claim that the DLL contains only open-source code or that its redistribution has
been cleared. `BUILDINFO.json` lists the observed imports and remaining objects.
The installed Folderwing release is not replaced by creating this candidate.
