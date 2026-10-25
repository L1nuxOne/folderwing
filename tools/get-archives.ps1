<# Pinned, local static archive dependencies. Only helpers link these libraries. #>
param([string]$Zig,[string]$Destination)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$dest=if($Destination){$Destination}else{Join-Path $root 'third_party\archives'}
if(-not $Zig){$Zig=Join-Path $env:USERPROFILE 'tools\zig-x86_64-windows-0.17.0\zig.exe'}
$lockFile=Join-Path $PSScriptRoot 'archive-dependencies.json'
$lock=Get-Content -LiteralPath $lockFile -Raw|ConvertFrom-Json
$lockHash=(Get-FileHash -LiteralPath $lockFile -Algorithm SHA256).Hash.ToLowerInvariant()
$compilerHash=(Get-FileHash -LiteralPath $Zig -Algorithm SHA256).Hash.ToLowerInvariant()
if(Test-Path -LiteralPath $dest){
 $stamp=Get-Content -LiteralPath (Join-Path $dest 'VERSION.openpilot.json') -Raw|ConvertFrom-Json
 if($stamp.lock_sha256 -ne $lockHash -or $stamp.compiler_sha256 -ne $compilerHash -or $stamp.configuration -ne $lock.configuration){throw 'Archive dependency cache does not match the pinned configuration/compiler. Use a fresh -Destination to rebuild.'}
 foreach($file in $stamp.files.psobject.Properties){$path=Join-Path $dest $file.Name;if(-not(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Value){throw "Archive dependency checksum mismatch: $($file.Name)"}}
 Write-Host 'Archive dependencies verified';return
}
$stage="$dest.stage-$PID"
$work=Join-Path $stage 'work';$prefix=Join-Path $stage 'prefix'
New-Item -ItemType Directory -Force $work,$prefix|Out-Null
function Invoke-ArchiveTool([string]$Exe,[string[]]$Arguments,[string]$Log){
 $pi=[Diagnostics.ProcessStartInfo]::new();$pi.FileName=$Exe;$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
 foreach($argument in $Arguments){[void]$pi.ArgumentList.Add($argument)}
 $process=[Diagnostics.Process]::Start($pi);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();$process.WaitForExit();$exit=$process.ExitCode;$process.Dispose()
 [IO.File]::WriteAllText($Log,$stdout.Result+$stderr.Result)
 if($exit -ne 0){throw "Archive dependency tool failed; see $Log"}
}
# Failed staging is retained for diagnosis; never delete a pre-existing dependency directory.
foreach($item in $lock.downloads){
 $zip=$item.url.EndsWith('.zip');$archive=Join-Path $work ($item.name+$(if($zip){'.zip'}else{'.tgz'}))
 Invoke-ArchiveTool 'curl.exe' @('-sSfL','--retry','0','-o',$archive,$item.url) (Join-Path $work ($item.name+'-download.log'))
 if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256){throw "Archive dependency download checksum mismatch: $($item.name)"}
 $source=Join-Path $work $item.name;New-Item -ItemType Directory $source|Out-Null
 if($zip){[IO.Compression.ZipFile]::ExtractToDirectory($archive,$source)}else{Invoke-ArchiveTool 'tar.exe' @('-xzf',$archive,'-C',$source,'--strip-components=1') (Join-Path $work ($item.name+'-extract.log'))}
}
$cm=Join-Path $work 'cmake\cmake-4.4.4-windows-x86_64\bin\cmake.exe';$ninja=Join-Path $work 'ninja\ninja.exe'
$cc=Join-Path $work 'cc.cmd';$ar=Join-Path $work 'ar.cmd';$ranlib=Join-Path $work 'ranlib.cmd'
[IO.File]::WriteAllText($cc,"@echo off`r`n`"$Zig`" cc -target x86_64-windows-gnu %*`r`n")
[IO.File]::WriteAllText($ar,"@echo off`r`n`"$Zig`" ar %*`r`n")
[IO.File]::WriteAllText($ranlib,"@echo off`r`n`"$Zig`" ranlib %*`r`n")
function Build-ArchiveLibrary([string]$Name,[string[]]$Options){
 Write-Host "Building archive dependency $Name"
 $binary=Join-Path $work ($Name+'-build')
 # CMake macros can reinterpret Windows backslashes as escapes (notably
 # ZLIB_LIBRARY passed through libarchive's TRY_MACRO_FOR_LIBRARY). Normalize
 # path-bearing configure arguments before CMake caches or expands them.
 $configure=@('-S',(Join-Path $work $Name),'-B',$binary,'-G','Ninja',"-DCMAKE_C_COMPILER=$cc","-DCMAKE_AR=$ar","-DCMAKE_RANLIB=$ranlib","-DCMAKE_MAKE_PROGRAM=$ninja",'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG','-DBUILD_SHARED_LIBS=OFF',"-DCMAKE_INSTALL_PREFIX=$prefix","-DCMAKE_PREFIX_PATH=$prefix")+$Options
 $configure=@($configure|ForEach-Object {$_.Replace('\','/')})
 Invoke-ArchiveTool $cm $configure (Join-Path $work ($Name+'-configure.log'))
 Invoke-ArchiveTool $cm @('--build',$binary,'--parallel','4') (Join-Path $work ($Name+'-build.log'))
 Invoke-ArchiveTool $cm @('--install',$binary) (Join-Path $work ($Name+'-install.log'))
}
Build-ArchiveLibrary 'miniz' @('-DBUILD_EXAMPLES=OFF','-DBUILD_TESTS=OFF')
Build-ArchiveLibrary 'zlib' @('-DZLIB_BUILD_TESTING=OFF','-DZLIB_BUILD_SHARED=OFF','-DZLIB_BUILD_STATIC=ON')
Build-ArchiveLibrary 'xz' @('-DXZ_TOOL_XZ=OFF','-DXZ_TOOL_XZDEC=OFF','-DXZ_TOOL_LZMADEC=OFF','-DXZ_TOOL_LZMAINFO=OFF','-DBUILD_TESTING=OFF','-DXZ_DOC=OFF','-DXZ_NLS=OFF')
$objects=@()
foreach($name in @('blocksort','huffman','crctable','randtable','compress','decompress','bzlib')){
 $object=Join-Path $work ($name+'.o');Invoke-ArchiveTool $Zig @('cc','-target','x86_64-windows-gnu','-O2','-DNDEBUG','-c',(Join-Path $work "bzip2\$name.c"),'-o',$object) (Join-Path $work ($name+'-compile.log'));$objects+=$object
}
Invoke-ArchiveTool $Zig (@('ar','rcs',(Join-Path $prefix 'lib\libbz2.a'))+$objects) (Join-Path $work 'bzip2-ar.log')
Copy-Item (Join-Path $work 'bzip2\bzlib.h') (Join-Path $prefix 'include')
Build-ArchiveLibrary 'libarchive' @("-DZLIB_LIBRARY=$prefix\lib\libzs.a","-DZLIB_INCLUDE_DIR=$prefix\include",'-DENABLE_TEST=OFF','-DENABLE_TAR=OFF','-DENABLE_CPIO=OFF','-DENABLE_CAT=OFF','-DENABLE_UNZIP=OFF','-DENABLE_OPENSSL=OFF','-DENABLE_MBEDTLS=OFF','-DENABLE_NETTLE=OFF','-DENABLE_LIBB2=OFF','-DENABLE_LZ4=OFF','-DENABLE_LZO=OFF','-DENABLE_ZSTD=OFF','-DENABLE_LIBXML2=OFF','-DENABLE_EXPAT=OFF','-DENABLE_BSDXML=OFF','-DENABLE_WIN32_XMLLITE=OFF','-DENABLE_PCREPOSIX=OFF','-DENABLE_PCRE2POSIX=OFF','-DENABLE_ICONV=OFF','-DENABLE_LZMA=ON','-DENABLE_BZip2=ON','-DENABLE_ZLIB=ON','-DENABLE_CNG=ON')
$licenses=Join-Path $prefix 'licenses';New-Item -ItemType Directory $licenses|Out-Null
$licenseSources=@{'miniz\LICENSE'='miniz-LICENSE.txt';'libarchive\COPYING'='libarchive-LICENSE.txt';'zlib\LICENSE'='zlib-LICENSE.txt';'bzip2\LICENSE'='bzip2-LICENSE.txt';'xz\COPYING'='xz-COPYING.txt';'xz\COPYING.0BSD'='xz-LICENSE.txt'}
foreach($name in $licenseSources.Keys){Copy-Item (Join-Path $work $name) (Join-Path $licenses $licenseSources[$name])}
$files=@{};Get-ChildItem $prefix -Recurse -File|ForEach-Object {$relative=[IO.Path]::GetRelativePath($prefix,$_.FullName).Replace('\','/');$files[$relative]=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
@{configuration=$lock.configuration;lock_sha256=$lockHash;compiler_sha256=$compilerHash;files=$files}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $prefix 'VERSION.openpilot.json')
Move-Item -LiteralPath $prefix -Destination $dest
Write-Host 'Archive dependencies built and pinned; build work retained in staging.'
