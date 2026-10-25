<#
.SYNOPSIS
  Builds the Folderwing engine, CLI, demo app and tests with `zig cc`.
.EXAMPLE
  ./build.ps1                 # release build into build/release
  ./build.ps1 -Config debug   # debug build into build/debug
  ./build.ps1 -Test           # build, then run the test suite
#>
param(
    [ValidateSet('release', 'debug')] [string] $Config = 'release',
    [switch] $Test,
    [string] $OutputDirectory,
    [string[]] $Targets
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$zig = $env:ZIG
if (-not $zig) {
    $cmd = Get-Command zig -ErrorAction SilentlyContinue
    $zig = if ($cmd) { $cmd.Source } else { Join-Path $env:USERPROFILE 'tools\zig-x86_64-windows-0.17.0\zig.exe' }
}
if (-not (Test-Path $zig)) { throw "zig not found. Install Zig and put it on PATH, or set `$env:ZIG." }

$out = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $root "build\$Config" }
New-Item -ItemType Directory -Force $out | Out-Null

$cflags = @('-target', 'x86_64-windows-gnu', '-std=c11', '-municode',
            '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
            "-I$root\include", '-DUNICODE', '-D_UNICODE')
if ($Config -eq 'release') { $cflags += @('-O2', '-DNDEBUG', '-s') } else { $cflags += @('-O0', '-g') }

$engine = Get-ChildItem "$root\src\*.c" | ForEach-Object FullName
$libs = @('-ladvapi32', '-lkernel32', '-luser32', '-lshell32', '-lole32', '-lmpr')

. "$root\tools\build-output.ps1"
function Invoke-Zig([string] $name, [string[]] $args_) {
    if ($Targets -and $name -notin $Targets -and -not $name.EndsWith('.o')) { return }
    Invoke-BuildCompiler -Compiler $zig -OutputDirectory $out -Name $name -Arguments $args_
}

Write-Host "Building ($Config) -> $out"
# The WSL helper: a static Linux program, run inside distros with wsl.exe (src/wsl.c)
& "$root\tools\get-sqlite.ps1"
$sqlite = Join-Path $root 'third_party\sqlite'
$lflags = @('-target', 'x86_64-linux-musl', '-static', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
            '-Wno-missing-field-initializers', "-I$root\src", "-I$sqlite", '-DSQLITE_ENABLE_FTS5', '-DSQLITE_OMIT_LOAD_EXTENSION', '-DSQLITE_THREADSAFE=1')
if ($Config -eq 'release') { $lflags += @('-O2', '-s') } else { $lflags += @('-O0', '-g') }
Invoke-Zig 'folderwing-wsl' ($lflags + @("$root\linux\fpwsl.c", "$root\linux\fppath.c", "$root\linux\fppermissions.c", "$root\linux\fpsizes.c", "$root\linux\fpspace.c", "$root\linux\fpduplicate.c", "$root\linux\fpgit.c", "$root\linux\fpcontent.c", "$root\app\contentmatch.c", "$root\app\textutil.c", "$root\linux\fpsearch.c", "$root\linux\fpindex.c", "$sqlite\sqlite3.c", "$root\linux\fphash.c", "$root\linux\fpguard.c", "$root\linux\fprename.c", "$root\linux\fpjobs.c", "$root\linux\fptransfer.c", "$root\linux\fpshell.c", '-lm', '-lpthread', '-o', "$out\folderwing-wsl"))
Invoke-Zig 'folderwing-engine.dll' ($cflags + @('-shared', '-DFPE_BUILD_DLL') + $engine + $libs + @('-o', "$out\folderwing-engine.dll"))
# On-demand elevated, directory-flush-only destination ancestry broker.
Invoke-Zig 'folderwing-durable.exe' ($cflags + @("$root\tools\fpdurable.c") + @('-lkernel32','-ladvapi32','-Wl,--subsystem,windows','-o',"$out\folderwing-durable.exe"))
Invoke-Zig 'folderwing-cli.exe' ($cflags + $engine + @("$root\tools\fpe.c") + $libs + @('-o', "$out\folderwing-cli.exe"))
Invoke-Zig 'fpe_tests.exe' ($cflags + $engine + @("$root\tests\tests.c") + $libs + @('-o', "$out\fpe_tests.exe"))
Invoke-Zig 'cli_tests.exe' ($cflags + $engine + @("$root\tests\cli_tests.c") + $libs + @('-o', "$out\cli_tests.exe"))
Invoke-Zig 'jobwait_tests.exe' ($cflags + $engine + @("$root\tests\jobwait_tests.c") + $libs + @('-o', "$out\jobwait_tests.exe"))
# This harness requires two owned distro paths; tests/cross_distro.ps1 runs it.
Invoke-Zig 'cross_tests.exe' ($cflags + $engine + @("$root\tests\cross_tests.c") + $libs + @('-o', "$out\cross_tests.exe"))
# Windows stream durability faults wrap only this executable's included writer.
$transferEngine = $engine | Where-Object { (Split-Path $_ -Leaf) -ne 'wsl.c' }
Invoke-Zig 'windows_transfer_tests.exe' ($cflags + $transferEngine + @("$root\tests\windows_transfer_tests.c") + $libs + @('-o', "$out\windows_transfer_tests.exe"))
Invoke-Zig 'windows_directory_tests.exe' ($cflags + $transferEngine + @("$root\tests\windows_directory_tests.c") + $libs + @('-o', "$out\windows_directory_tests.exe"))
$treeEngine = $engine | Where-Object { (Split-Path $_ -Leaf) -ne 'pathguard.c' }
Invoke-Zig 'tree_durability_tests.exe' ($cflags + $treeEngine + @("$root\tests\tree_durability_tests.c") + $libs + @('-o', "$out\tree_durability_tests.exe"))
$nativeDurabilityEngine = $engine | Where-Object { (Split-Path $_ -Leaf) -notin @('jobs.c','pathguard.c') }
Invoke-Zig 'native_durability_tests.exe' ($cflags + $nativeDurabilityEngine + @("$root\tests\native_durability_tests.c") + $libs + @('-o', "$out\native_durability_tests.exe"))
$ancestryEngine = $engine | Where-Object { (Split-Path $_ -Leaf) -notin @('jobs.c','durable.c') }
Invoke-Zig 'windows_ancestry_tests.exe' ($cflags + $ancestryEngine + @("$root\tests\windows_ancestry_tests.c") + $libs + @('-o', "$out\windows_ancestry_tests.exe"))
# Permanent-delete faults are private to the included jobs translation unit.
$deleteEngine = $engine | Where-Object { (Split-Path $_ -Leaf) -ne 'jobs.c' }
Invoke-Zig 'windows_delete_tests.exe' ($cflags + $deleteEngine + @("$root\tests\windows_delete_tests.c") + $libs + @('-o', "$out\windows_delete_tests.exe"))
Invoke-Zig 'durable_helper_tests.exe' ($cflags + @("$root\tests\durable_helper_tests.c") + @('-lkernel32','-ladvapi32','-o',"$out\durable_helper_tests.exe"))
# Archive codecs are linked only into their on-demand restricted helper executables.
& "$root\tools\get-archives.ps1" -Zig $zig
$archive = Join-Path $root 'third_party\archives'
$archiveFlags = $cflags + @("-I$archive\include", "-I$archive\include\miniz")
Invoke-Zig 'folderwing-zip.exe' ($archiveFlags + @('-DARCP_MINIZ',"$root\app\fparchive.c","$root\app\archiveutil.c","$archive\lib\libminiz.a") + @('-lkernel32','-lmpr','-o',"$out\folderwing-zip.exe"))
Invoke-Zig 'folderwing-archive.exe' ($archiveFlags + @("$root\app\fparchive.c","$root\app\archiveutil.c","$archive\lib\libarchive.a","$archive\lib\libzs.a","$archive\lib\libbz2.a","$archive\lib\liblzma.a") + @('-lkernel32','-lbcrypt','-lcrypt32','-luser32','-ladvapi32','-lole32','-luuid','-lmpr','-o',"$out\folderwing-archive.exe"))
foreach($license in Get-ChildItem "$archive\licenses" -File){Publish-BuildAsset $license.FullName $out ('archive-'+$license.Name)}
$app = @("$root\app\brand.c", "$root\app\actiondock.c", "$root\app\gitmodel.c", "$root\app\mediautil.c", "$root\app\mediaview.c", "$root\app\workspace.c", "$root\app\workspaces.c", "$root\app\archiveutil.c", "$root\app\archiveclient.c", "$root\app\archiveview.c", "$root\app\dropwarm.c", "$root\app\imagebrowse.c", "$root\app\permissionutil.c", "$root\app\linkdialog.c", "$root\app\permissions.c", "$root\app\foldersize.c", "$root\app\foldercompare.c", "$root\app\bulkrename.c", "$root\app\renameutil.c", "$root\app\pathentry.c", "$root\app\termutil.c", "$root\app\fpdemo.c", "$root\app\shell.c", "$root\app\fn.c", "$root\app\functions.c", "$root\app\session.c", "$root\app\folderview.c",
         "$root\app\folderwatch.c", "$root\app\palette.c", "$root\app\fuzzy.c", "$root\app\everything.c", "$root\app\typeicons.c",
         "$root\app\thumbnails.c", "$root\app\thumbutil.c", "$root\app\textview.c", "$root\app\textutil.c", "$root\app\imageview.c", "$root\app\imageutil.c", "$root\app\pdfview.c", "$root\app\pdftext.c", "$root\app\pdfpos.c", "$root\app\sidepanel.c", "$root\app\keys.c",
         "$root\app\menu.c", "$root\app\relay.c", "$root\app\relayedit.c", "$root\app\relaycmd.c", "$root\app\operations.c", "$root\app\previewlaunch.c", "$root\app\previewreader.c", "$root\app\pipewriter.c")
# Reproducible checked-in SVG/PNG/ICO exports; resource embeds icons and version metadata.
$brandObject = Join-Path $out 'folderwing-brand.o'
& $zig rc /fo $brandObject (Join-Path $root 'app\folderwing.rc')
if($LASTEXITCODE -ne 0){throw 'Folderwing resource compilation failed'}
Invoke-Zig 'folderwing.exe' ($cflags + $engine + $app + $libs + @($brandObject) +
    @('-lgdi32', '-lmsimg32', '-ladvapi32', '-ldwmapi', '-luxtheme', '-luuid', '-limm32', '-Wl,--subsystem,windows', '-o', "$out\folderwing.exe"))
# Media codecs stay outside the explorer and load only for selected media.
& "$root\tools\get-media.ps1"
$media = Join-Path $root 'third_party\mpv'
$mediaFlags = $cflags | Where-Object { $_ -ne '-std=c11' }
Invoke-Zig 'mediautil.o' ($cflags + @('-c',"$root\app\mediautil.c",'-o',"$out\mediautil.o"))
Invoke-Zig 'folderwing-media.exe' ($mediaFlags + @('-std=c++17','-fno-exceptions','-fno-rtti','-fms-extensions',"-I$media\include","$root\app\fpmedia.cpp","$root\app\mediaengine.cpp","$out\mediautil.o") + @('-lkernel32','-luser32','-lole32','-loleaut32','-luuid','-ld3d11','-ldxgi','-ldxguid','-lmfplat','-lmfuuid','-lmf','-lpropsys','-Wl,--subsystem,windows','-o',"$out\folderwing-media.exe"))
# Force all supported video codec fixtures through the real fallback renderer.
Invoke-Zig 'media_codec_tests.exe' ($mediaFlags + @('-x','c++','-std=c++17','-fno-exceptions','-fno-rtti','-fms-extensions',"-I$media\include","$root\tests\media_codec_tests.c","$root\app\mediaengine.cpp",'-x','none',"$out\mediautil.o") + @('-lkernel32','-luser32','-lole32','-loleaut32','-luuid','-ld3d11','-ldxgi','-ldxguid','-lmfplat','-lmfuuid','-lmf','-lpropsys','-o',"$out\media_codec_tests.exe"))
# Publish every pinned runtime notice alongside the replaceable LGPL DLL.
& "$root\tools\copy-media-assets.ps1" -OutputDirectory $out
Invoke-Zig 'media_tests.exe' ($cflags + @("$root\tests\media_tests.c","$root\app\mediaview.c","$root\app\mediautil.c","$root\app\previewlaunch.c","$root\app\previewreader.c","$root\app\pipewriter.c") + @('-lkernel32','-luser32','-lgdi32','-ladvapi32','-o',"$out\media_tests.exe"))
Invoke-Zig 'media_audio_tests.exe' ($cflags + @("$root\tests\media_audio_tests.c","$root\app\mediaview.c","$root\app\mediautil.c","$root\app\previewlaunch.c","$root\app\previewreader.c","$root\app\pipewriter.c") + @('-lkernel32','-luser32','-lgdi32','-ladvapi32','-o',"$out\media_audio_tests.exe"))
$mediaHostApp = $app | Where-Object { $_ -notin @("$root\app\fpdemo.c","$root\app\archiveview.c") }
Invoke-Zig 'media_host_tests.exe' ($cflags + $engine + $mediaHostApp + @("$root\tests\media_host_tests.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\media_host_tests.exe"))
# Archive acceptance includes the actual host and archive window; the other app sources stay ordinary.
$archiveTestApp = $app | Where-Object { $_ -notin @("$root\app\fpdemo.c", "$root\app\archiveview.c", "$root\app\archiveclient.c", "$root\app\imageview.c") }
Invoke-Zig 'archive_tests.exe' ($cflags + $engine + $archiveTestApp + @("$root\tests\archive_tests.c", "$root\tests\fixtures\native\archive_image_probe.c", "$root\tests\fixtures\native\archive_cleanup_probe.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\archive_tests.exe"))
# Space host integration includes the real explorer and its actual panel helper.
Invoke-Zig 'workspace_tests.exe' ($cflags + @("$root\tests\workspace_tests.c","$root\app\workspace.c","$root\app\session.c") + @('-lkernel32','-luser32','-o',"$out\workspace_tests.exe"))
$brandingHostApp = $app | Where-Object { $_ -ne "$root\app\fpdemo.c" }
Invoke-Zig 'branding_host_tests.exe' ($cflags + $engine + $brandingHostApp + @("$root\tests\branding_host_tests.c",$brandObject) + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-lversion','-o',"$out\branding_host_tests.exe"))
$wslTreeHostApp = $app | Where-Object { $_ -ne "$root\app\fpdemo.c" }
Invoke-Zig 'wsl_tree_host_tests.exe' ($cflags + $engine + $wslTreeHostApp + @("$root\tests\wsl_tree_host_tests.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\wsl_tree_host_tests.exe"))
$workspaceHostApp = $app | Where-Object { $_ -notin @("$root\app\fpdemo.c", "$root\app\folderwatch.c") }
Invoke-Zig 'workspace_host_tests.exe' ($cflags + $engine + $workspaceHostApp + @("$root\tests\workspace_host_tests.c", "$root\tests\fixtures\native\workspace_watch_probe.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\workspace_host_tests.exe"))
$spaceHostApp = $app | Where-Object { $_ -ne "$root\app\fpdemo.c" }
Invoke-Zig 'space_host_tests.exe' ($cflags + $engine + $spaceHostApp + @("$root\tests\space_host_tests.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\space_host_tests.exe"))
# Source-included host with a harness-only observer of the real text reader.
$contentHostApp = $app | Where-Object { $_ -notin @("$root\app\fpdemo.c","$root\app\textview.c") }
Invoke-Zig 'content_host_tests.exe' ($cflags + $engine + $contentHostApp + @("$root\tests\content_host_tests.c","$root\tests\fixtures\native\content_text_probe.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\content_host_tests.exe"))
# Native drag integration includes the host source and clipboard-wrapped shell source.
$dragApp = $app | Where-Object { $_ -notin @("$root\app\fpdemo.c", "$root\app\shell.c") }
Invoke-Zig 'drag_tests.exe' ($cflags + $engine + $dragApp + @("$root\tests\drag_tests.c") + $libs +
    @('-lgdi32', '-lmsimg32', '-ladvapi32', '-ldwmapi', '-luxtheme', '-luuid', '-limm32', '-o', "$out\drag_tests.exe"))
# Deterministic process-creation stalls exercise panel cancellation and job containment.
Invoke-Zig 'panel_tests.exe' ($cflags + @("$root\tests\panel_tests.c", "$root\src\prof.c") +
    @('-lkernel32', '-luser32', '-lgdi32', '-limm32', '-o', "$out\panel_tests.exe"))
Invoke-Zig 'panel_paint_tests.exe' ($cflags + @("$root\tests\panel_paint_tests.c", "$root\src\prof.c") +
    @('-lkernel32', '-luser32', '-lgdi32', '-limm32', '-o', "$out\panel_paint_tests.exe"))
Invoke-Zig 'preview_lifecycle_tests.exe' ($cflags + @("$root\tests\preview_lifecycle_tests.c", "$root\app\pipewriter.c", "$root\app\previewreader.c") +
    @('-lkernel32', '-luser32', '-ladvapi32', '-o', "$out\preview_lifecycle_tests.exe"))
# The shell helper: relays to Explorer's menu items and Send to run here (app/relay.h)
Invoke-Zig 'watch_tests.exe' ($cflags + @("$root\tests\watch_tests.c") +
    @('-lkernel32', '-luser32', '-o', "$out\watch_tests.exe"))
Invoke-Zig 'folderwing-shell.exe' ($cflags + @("$root\app\fpshell.c", "$root\app\relaycmd.c") +
    @('-lkernel32', '-luser32', '-lshell32', '-lole32', '-luuid', '-ladvapi32', '-Wl,--subsystem,windows', '-o', "$out\folderwing-shell.exe"))
Invoke-Zig 'shell_protocol_tests.exe' ($cflags + @("$root\tests\shell_protocol_tests.c", "$root\app\relaycmd.c") +
    @('-lkernel32', '-luser32', '-lshell32', '-lole32', '-luuid', '-ladvapi32', '-o', "$out\shell_protocol_tests.exe"))
# Content search remains entirely on demand in its panel/native Linux helper.
Invoke-Zig 'folderwing-diff.exe' ($cflags + @("$root\app\fpdiff.c","$root\app\gitread.c","$root\app\gitmodel.c","$root\app\diff.c","$root\app\diffread.c","$root\app\textutil.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lmpr','-Wl,--subsystem,windows','-o',"$out\folderwing-diff.exe"))
Invoke-Zig 'provider_admission_tests.exe' ($cflags + @("$root\tests\provider_admission_tests.c","$root\app\archiveutil.c","$root\app\previewlaunch.c","$root\app\diff.c","$root\app\textutil.c") + @('-lkernel32','-luser32','-ladvapi32','-lole32','-lshell32','-lmpr','-o',"$out\provider_admission_tests.exe"))
Invoke-Zig 'diff_tests.exe' ($cflags + @("$root\tests\diff_tests.c","$root\app\gitread.c","$root\app\gitmodel.c","$root\app\diff.c","$root\app\diffread.c","$root\app\textutil.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lmpr','-o',"$out\diff_tests.exe"))
$diffHostApp = $app | Where-Object { $_ -ne "$root\app\fpdemo.c" }
Invoke-Zig 'diff_host_tests.exe' ($cflags + $engine + $diffHostApp + @("$root\tests\diff_host_tests.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\diff_host_tests.exe"))
Invoke-Zig 'folderwing-content.exe' ($cflags + @("$root\app\fpcontent.c","$root\app\contentmodel.c","$root\app\contentscan.c","$root\app\contentmatch.c","$root\app\textutil.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lmpr','-Wl,--subsystem,windows','-o',"$out\folderwing-content.exe"))
Invoke-Zig 'content_tests.exe' ($cflags + @("$root\tests\content_tests.c","$root\app\contentmodel.c","$root\app\contentscan.c","$root\app\contentmatch.c","$root\app\textutil.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lmpr','-o',"$out\content_tests.exe"))
# Space analyzer: metadata traversal and all result rendering stay in the panel helper.
Invoke-Zig 'actiondock_tests.exe' ($cflags + @("$root\tests\actiondock_tests.c","$root\app\actiondock.c") + @('-lkernel32','-luser32','-lgdi32','-o',"$out\actiondock_tests.exe"))
$actionDockHostApp = $app | Where-Object { $_ -ne "$root\app\fpdemo.c" }
Invoke-Zig 'actiondock_host_tests.exe' ($cflags + $engine + $actionDockHostApp + @("$root\tests\actiondock_host_tests.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\actiondock_host_tests.exe"))
Invoke-Zig 'folderwing-git.exe' ($cflags + @("$root\app\fpgit.c","$root\app\gitread.c","$root\app\gitmodel.c","$root\app\diff.c","$root\app\textutil.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lmpr','-Wl,--subsystem,windows','-o',"$out\folderwing-git.exe"))
Invoke-Zig 'git_tests.exe' ($cflags + @("$root\tests\git_tests.c","$root\app\gitread.c","$root\app\gitmodel.c","$root\app\diff.c","$root\app\textutil.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lmpr','-o',"$out\git_tests.exe"))
$gitHostApp = $app | Where-Object { $_ -ne "$root\app\fpdemo.c" }
Invoke-Zig 'git_host_tests.exe' ($cflags + $engine + $gitHostApp + @("$root\tests\git_host_tests.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\git_host_tests.exe"))
Invoke-Zig 'folderwing-duplicates.exe' ($cflags + @("$root\app\fpduplicates.c","$root\app\duplicates.c","$root\app\duplicateread.c","$root\app\space.c","$root\app\spacescan.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lmpr','-Wl,--subsystem,windows','-o',"$out\folderwing-duplicates.exe"))
Invoke-Zig 'duplicate_tests.exe' ($cflags + @("$root\tests\duplicate_tests.c","$root\app\duplicates.c","$root\app\duplicateread.c","$root\app\space.c","$root\app\spacescan.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lmpr','-o',"$out\duplicate_tests.exe"))
Invoke-Zig 'duplicate_mount_tests.exe' ($cflags + @("$root\tests\duplicate_mount_tests.c","$root\app\duplicates.c","$root\app\duplicateread.c","$root\app\space.c","$root\app\spacescan.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lmpr','-o',"$out\duplicate_mount_tests.exe"))
Invoke-Zig 'duplicate_scope_tests.exe' ($cflags + @("$root\tests\duplicate_scope_tests.c","$root\app\duplicateread.c","$root\app\space.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lmpr','-o',"$out\duplicate_scope_tests.exe"))
$duplicateHostApp = $app | Where-Object { $_ -ne "$root\app\fpdemo.c" }
Invoke-Zig 'duplicate_host_tests.exe' ($cflags + $engine + $duplicateHostApp + @("$root\tests\duplicate_host_tests.c") + $libs + @('-lgdi32','-lmsimg32','-ladvapi32','-ldwmapi','-luxtheme','-luuid','-limm32','-o',"$out\duplicate_host_tests.exe"))
# Elevated read-only NTFS metadata and private per-user cache worker.
Invoke-Zig 'ntfs_panel_tests.exe' ($cflags + @("$root\tests\ntfs_panel_tests.c","$root\app\space.c","$root\app\spacescan.c","$root\app\ntfsclient.c","$root\app\termutil.c") + @('-lkernel32','-ladvapi32','-lshell32','-lole32','-luser32','-lgdi32','-lmpr','-o',"$out\ntfs_panel_tests.exe"))
Invoke-Zig 'ntfs_model_tests.exe' ($cflags + @("$root\tests\ntfs_model_tests.c","$root\app\ntfsmft.c","$root\app\space.c","$root\app\ntfsclient.c") + @('-lkernel32','-ladvapi32','-lbcrypt','-lshell32','-lole32','-luser32','-o',"$out\ntfs_model_tests.exe"))
Invoke-Zig 'folderwing-ntfs.exe' ($cflags + @("$root\app\fpntfs.c","$root\app\ntfscache.c","$root\app\ntfsmft.c") + @('-lkernel32','-ladvapi32','-lbcrypt','-o',"$out\folderwing-ntfs.exe"))
Invoke-Zig 'folderwing-space.exe' ($cflags + @("$root\app\fpspace.c","$root\app\space.c","$root\app\spacescan.c","$root\app\ntfsclient.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lshell32','-ladvapi32','-lmpr','-Wl,--subsystem,windows','-o',"$out\folderwing-space.exe"))
Invoke-Zig 'space_tests.exe' ($cflags + @("$root\tests\space_tests.c","$root\app\space.c","$root\app\spacescan.c","$root\app\ntfsclient.c","$root\app\termutil.c") + @('-lkernel32','-luser32','-lgdi32','-lshell32','-ladvapi32','-lmpr','-o',"$out\space_tests.exe"))
# Operations: a panel app; jobs and retained results stay in the explorer.
Invoke-Zig 'folderwing-operations.exe' ($cflags + @("$root\app\fpoperations.c", "$root\app\operations.c") +
    @('-lkernel32', '-luser32', '-lgdi32', '-Wl,--subsystem,windows', '-o', "$out\folderwing-operations.exe"))
# The settings app, shown in the side panel (app/panelproto.h)
Invoke-Zig 'folderwing-settings.exe' ($cflags + @('-DRELAY_FORM_ONLY', "$root\app\fpsettings.c", "$root\app\keys.c", "$root\app\relayedit.c", "$root\app\relaycmd.c", "$root\app\relaycatalog.c") +
    @('-lkernel32', '-luser32', '-lgdi32', '-lole32', '-luuid', '-ladvapi32', '-Wl,--subsystem,windows', '-o', "$out\folderwing-settings.exe"))
Invoke-Zig 'dropwarm_tests.exe' ($cflags + @("$root\tests\dropwarm_tests.c", "$root\src\prof.c") +
    @('-lkernel32', '-luser32', '-lole32', '-luuid', '-o', "$out\dropwarm_tests.exe"))
Invoke-Zig 'app_tests.exe' ($cflags + $engine + @("$root\tests\app_tests.c", "$root\app\renameutil.c", "$root\app\permissionutil.c", "$root\app\linkdialog.c", "$root\app\pathentry.c", "$root\app\shell.c", "$root\app\fn.c", "$root\app\keys.c", "$root\app\session.c", "$root\app\folderview.c",
                                                  "$root\app\fuzzy.c", "$root\app\everything.c", "$root\app\typeicons.c", "$root\app\pdftext.c", "$root\app\pdfpos.c",
                                                  "$root\app\relaycmd.c", "$root\app\relayedit.c", "$root\app\relaycatalog.c", "$root\app\termutil.c", "$root\app\operations.c", "$root\app\imagebrowse.c", "$root\app\imageutil.c", "$root\app\textutil.c", "$root\app\thumbutil.c") +
    $libs + @('-luuid', '-lgdi32', '-ladvapi32', '-o', "$out\app_tests.exe"))

# Text preview: bounded decoding, highlighting, layout and search in an isolated helper.
Invoke-Zig 'folderwing-text.exe' ($cflags + @("$root\app\fptext.c", "$root\app\textutil.c") +
    @('-lkernel32', '-luser32', '-Wl,--subsystem,windows', '-o', "$out\folderwing-text.exe"))

# Image preview uses WIC and a pinned, bundled WebP decoder. No silent WIC-only build.
$webp = Join-Path $root 'third_party\libwebp'
if (-not (Test-Path "$webp\src\webp\decode.h")) { & "$root\tools\get-libwebp.ps1" }
$websrc = @()
foreach ($dir in @('dec','demux')) { $websrc += Get-ChildItem "$webp\src\$dir\*.c" | ForEach-Object FullName }
$dsp = @('alpha_processing','cpu','dec','dec_clip_tables','filters','lossless','rescaler','upsampling','yuv')
$utils = @('bit_reader_utils','color_cache_utils','filters_utils','huffman_utils','palette','quant_levels_dec_utils','rescaler_utils','random_utils','thread_utils','utils')
$websrc += Get-ChildItem "$webp\src\dsp\*_sse2.c" | Where-Object { $_.BaseName -notmatch '^(enc|cost|ssim|lossless_enc)' } | ForEach-Object FullName
$websrc += $dsp | ForEach-Object { "$webp\src\dsp\$_.c" }
$websrc += $utils | ForEach-Object { "$webp\src\utils\$_.c" }
# Only the SVG adapter is C++; it uses COM without the C++ runtime.
$svgflags = $cflags | Where-Object { $_ -notin @('-std=c11', '-municode', '-s') }
Invoke-Zig 'svgview.o' ($svgflags + @('-std=c++17', '-fno-exceptions', '-fno-rtti', '-c', "$root\app\svgview.cpp", '-o', "$out\svgview.o"))
Invoke-Zig 'folderwing-image.exe'  ($cflags + @("-I$webp", '-Wno-sign-compare', "$root\app\fpimage.c", "$out\svgview.o", "$root\app\imageutil.c", "$root\app\textutil.c") + $websrc +
    @('-lkernel32', '-lole32', '-lwindowscodecs', '-luuid', '-lpropsys', '-ld3d11', '-ld2d1', '-ldxgi', '-lshlwapi', '-lxmllite', '-Wl,--subsystem,windows', '-o', "$out\folderwing-image.exe"))
Publish-BuildAsset "$webp\COPYING" $out 'libwebp-LICENSE.txt'
Publish-BuildAsset "$webp\PATENTS" $out 'libwebp-PATENTS.txt'
Assert-BuildArtifactSet $out @('folderwing-image.exe','libwebp-LICENSE.txt','libwebp-PATENTS.txt') @() @{
    'libwebp-LICENSE.txt' = "$webp\COPYING"; 'libwebp-PATENTS.txt' = "$webp\PATENTS"
}

# The PDF helper needs PDFium (./tools/get-pdfium.ps1); without it, PDF preview is just not available.
$pdfium = Join-Path $root 'third_party\pdfium'
if (Test-Path "$pdfium\bin\pdfium.dll") {
    if (-not (Test-Path "$pdfium\include\fpdfview.h") -or -not (Test-Path "$pdfium\lib\pdfium.dll.lib")) {
        throw 'PDFium input set is incomplete; restore the pinned dependency before building.'
    }
    Invoke-Zig 'folderwing-pdf.exe' ($cflags + @("-I$pdfium\include", "$root\app\fppdf.c", "$pdfium\lib\pdfium.dll.lib") +
        @('-lkernel32', '-Wl,--subsystem,windows', '-o', "$out\folderwing-pdf.exe"))
    Publish-BuildAsset "$pdfium\bin\pdfium.dll" $out 'pdfium.dll'
    Assert-BuildArtifactSet $out @('folderwing-pdf.exe','pdfium.dll') @() @{'pdfium.dll'="$pdfium\bin\pdfium.dll"}
} else {
    Retire-BuildArtifacts $out @('folderwing-pdf.exe','pdfium.dll')
    Assert-BuildArtifactSet $out @() @('folderwing-pdf.exe','pdfium.dll')
    Write-Host "  (no PDFium: PDF preview is not built; run ./tools/get-pdfium.ps1)"
}

# The terminal panel app needs libvterm (./tools/get-libvterm.ps1); without it there's no panel terminal.
$vterm = Join-Path $root 'third_party\libvterm'
if (Test-Path "$vterm\include\vterm.h") {
    $vsrc = Get-ChildItem "$vterm\src\*.c" | ForEach-Object FullName
    Invoke-Zig 'folderwing-terminal.exe' ($cflags + @("-I$vterm\include", '-Wno-sign-compare', '-Wno-unused-function', "$root\app\fpterm.c", "$root\app\termutil.c") + $vsrc +
        @('-lkernel32', '-luser32', '-lgdi32', '-lmpr', '-Wl,--subsystem,windows', '-o', "$out\folderwing-terminal.exe"))
    if ($Test) {
        Invoke-Zig 'terminal_tests.exe' ($cflags + @("-I$vterm\include", '-Wno-sign-compare', '-Wno-unused-function', "$root\tests\terminal_tests.c", "$root\app\termutil.c") + $vsrc +
            @('-lkernel32', '-luser32', '-lgdi32', '-lmpr', '-o', "$out\terminal_tests.exe"))
        Assert-BuildArtifactSet $out @('folderwing-terminal.exe','terminal_tests.exe') @()
    } else {
        Retire-BuildArtifacts $out @('terminal_tests.exe')
        Assert-BuildArtifactSet $out @('folderwing-terminal.exe') @('terminal_tests.exe')
    }
} else {
    Retire-BuildArtifacts $out @('folderwing-terminal.exe','terminal_tests.exe')
    Assert-BuildArtifactSet $out @() @('folderwing-terminal.exe','terminal_tests.exe')
    if ($Test) { throw 'Terminal tests require libvterm; run ./tools/get-libvterm.ps1, then build again.' }
    Write-Host "  (no libvterm: the panel terminal is not built; run ./tools/get-libvterm.ps1)"
}

Get-ChildItem $out -Filter *.pdb -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
Get-ChildItem $out -Include *.exe, *.dll, folderwing-wsl -Recurse |
    ForEach-Object { '  {0,-16} {1,8:N0} KB' -f $_.Name, ($_.Length / 1KB) } | Write-Host

if ($Test) {
    & "$out\fpe_tests.exe"
    $engineFailed = $LASTEXITCODE
    & "$out\cli_tests.exe"
    $cliFailed = $LASTEXITCODE
    & "$out\windows_delete_tests.exe"
    $deleteFailed = $LASTEXITCODE
    & "$out\jobwait_tests.exe"
    if ($LASTEXITCODE -ne 0) { throw "jobwait tests failed" }
    & "$out\dropwarm_tests.exe"
    $dropWarmFailed=$LASTEXITCODE
    & "$out\app_tests.exe"
    $appFailed = $LASTEXITCODE
    & "$out\shell_protocol_tests.exe"
    $shellProtocolFailed = $LASTEXITCODE
    & "$out\panel_tests.exe"
    $panelFailed = $LASTEXITCODE
    & "$out\panel_paint_tests.exe"
    $panelPaintFailed = $LASTEXITCODE
    & "$out\preview_lifecycle_tests.exe"
    $previewLifecycleFailed = $LASTEXITCODE
    & "$out\watch_tests.exe"
    $watchFailed = $LASTEXITCODE
    & "$out\terminal_tests.exe"
    $terminalFailed = $LASTEXITCODE
    & pwsh.exe -NoProfile -File "$root\tests\build_output.ps1" -Zig $zig
    exit ($deleteFailed + $cliFailed + $dropWarmFailed + $engineFailed + $appFailed + $shellProtocolFailed + $panelFailed + $panelPaintFailed + $previewLifecycleFailed + $watchFailed + $terminalFailed + $LASTEXITCODE)
}
