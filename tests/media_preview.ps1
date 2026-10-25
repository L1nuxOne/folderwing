<# Focused media acceptance. No physical input, global audio changes or WSL shutdown.
   Audio output is a separate explicit lane: -Audio, after the HDMI monitor wakes. #>
param([string]$BuildDirectory,[string]$ResultDirectory,[switch]$Audio,[string]$FixtureDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent;$overall=[Diagnostics.Stopwatch]::StartNew()
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build/release'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build/media_preview/'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a new result directory'};New-Item -ItemType Directory $ResultDirectory|Out-Null
$fixtures=Join-Path $ResultDirectory 'fixtures';$right=Join-Path $ResultDirectory 'right';New-Item -ItemType Directory $fixtures,$right|Out-Null
$source=if($FixtureDirectory){$FixtureDirectory}else{Join-Path $root 'build/media-fixtures'}
if(-not(Test-Path (Join-Path $source 'FIXTURES.json'))){throw 'Synthetic media fixtures are missing. Run python tests/make_media_fixtures.py --ffmpeg <ffmpeg.exe> --output-directory build/media-fixtures, or pass -FixtureDirectory.'}
$fixtureManifest=Get-Content (Join-Path $source 'FIXTURES.json') -Raw|ConvertFrom-Json
if($fixtureManifest.schema -ne 'folderwing-synthetic-media/v1' -or $fixtureManifest.personal_media_used){throw 'Expected owned synthetic fixtures'}
foreach($item in $fixtureManifest.files.psobject.Properties){if($item.Name -match '(^[/\\]|:|(^|[/\\])\.\.([/\\]|$))'){throw 'Unsafe fixture member'};if((Get-FileHash (Join-Path $source $item.Name)).Hash.ToLowerInvariant() -cne $item.Value){throw 'Synthetic media fixture changed'}}
foreach($name in @('h264_aac.mp4','h264_aac.mkv','h264_4k60.mp4','hevc_aac.mp4','av1_aac.mp4','vp8_vorbis.webm','vp9_opus.webm','mpeg4_pcm.avi')){Copy-Item (Join-Path $source ('video-only/'+$name)) (Join-Path $fixtures $name);Copy-Item (Join-Path $source ('video-only/'+$name)) (Join-Path $right $name)}
Copy-Item (Join-Path $fixtures 'h264_aac.mp4') (Join-Path $fixtures '你好 #100%.mp4')
Copy-Item (Join-Path $source 'fixtures/corrupt.mp4') (Join-Path $fixtures 'corrupt.mp4');[IO.File]::WriteAllText((Join-Path $fixtures 'aaa.txt'),'ordinary file');[IO.File]::WriteAllText((Join-Path $right 'aaa.txt'),'ordinary file')
$zip=[IO.Compression.ZipFile]::Open((Join-Path $fixtures 'media.zip'),[IO.Compression.ZipArchiveMode]::Create);try{[void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path $fixtures 'h264_aac.mp4'),'movie.mp4');[void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path $fixtures 'vp9_opus.webm'),'fallback.webm')}finally{$zip.Dispose()}
Copy-Item (Join-Path $fixtures 'media.zip') (Join-Path $fixtures 'media-other.zip')
function Invoke-OwnedMediaWsl([string[]]$Arguments){$pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $env:SystemRoot 'System32/wsl.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;foreach($arg in $Arguments){[void]$pi.ArgumentList.Add($arg)};$p=[Diagnostics.Process]::Start($pi);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(-not $p.WaitForExit(30000)){$p.Kill($true);throw 'Owned WSL fixture command timed out'};$exit=$p.ExitCode;$p.Dispose();if($exit){throw $stderr.Result};return $stdout.Result}
$linux='/tmp/openpilot-media-'+[guid]::NewGuid().ToString('N');$process=$null;$created=$false
$setup=@'
import os,sys,shutil
p=sys.argv[1];assert p.startswith('/tmp/openpilot-media-') and len(p.rsplit('-',1)[1])==32
os.mkdir(p,0o700);open(p+'/.owner','w').write('openpilot-media/v1')
shutil.copyfile(sys.argv[2],p+'/h264_aac.mp4');os.mkfifo(p+'/not-a-file.mp4')
print(os.environ['WSL_DISTRO_NAME'])
'@
try{
 $windowsMedia=Join-Path $fixtures 'h264_aac.mp4';if($windowsMedia -notmatch '^[A-Za-z]:\\'){throw 'Fixture must be on an owned Windows drive'};$linuxMedia='/mnt/'+$windowsMedia.Substring(0,1).ToLowerInvariant()+'/'+$windowsMedia.Substring(3).Replace('\','/')
 $distro=(Invoke-OwnedMediaWsl @('--exec','python3','-c',$setup,$linux,$linuxMedia)).Trim();$created=$true;if($distro -notmatch '^[A-Za-z0-9._-]+$'){throw 'Unexpected distro identity'};$unc='\\wsl.localhost\'+$distro+$linux.Replace('/','\')
 $modules=@('media_codec_tests.exe','media_tests.exe','media_host_tests.exe');if($Audio){$modules+=@('media_audio_tests.exe')}
 $images=[ordered]@{};foreach($name in ($modules+@('folderwing-media.exe','folderwing.exe','libmpv-2.dll'))){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $sources=[ordered]@{};foreach($file in @('app/fpmedia.cpp','app/mediaengine.cpp','app/mediaengine.h','app/mediautil.c','app/mediautil.h','app/mediaproto.h','app/mediaview.c','app/mediaview.h','app/fpdemo.c','app/thumbnails.c','app/thumbnails.h','tests/media_tests.c','tests/media_audio_tests.c','tests/media_host_tests.c','tests/fixtures/native/media_test_host.h','tests/media_preview.ps1','tools/media-dependencies.json','build.ps1')){$sources[$file]=(Get-FileHash (Join-Path $root $file) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json
 $selected=@($registry.selectors.psobject.Properties|Where-Object {$module=$_.Name.Split('::')[0];$module -in @('tests/media_codec_tests.c','tests/media_tests.c','tests/media_host_tests.c') -or ($Audio -and $module -eq 'tests/media_audio_tests.c')})
 foreach($selector in $selected){foreach($file in $selector.Value.sources){$path=Join-Path $root $file;if(Test-Path $path -PathType Leaf){$sources[$file]=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()}}}
 foreach($name in @('folderwing-zip.exe','folderwing-archive.exe','folderwing-wsl','folderwing-text.exe','folderwing-image.exe','folderwing-pdf.exe','pdfium.dll')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $os=Get-CimInstance Win32_OperatingSystem;$profile=[ordered]@{workers=1;pwsh=$PSVersionTable.PSVersion.ToString();os=[Environment]::OSVersion.VersionString;free_memory_kib=$os.FreePhysicalMemory;cpu_load=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage);cache='uncontrolled development host';containers='unchanged; unused';network='local WSL only';external_services='none'};$setupMs=$overall.Elapsed.TotalMilliseconds
 $fixtureHashes=[ordered]@{};foreach($file in Get-ChildItem $fixtures -File){$fixtureHashes[$file.Name]=(Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()};if($Audio){foreach($file in Get-ChildItem (Join-Path $source 'fixtures') -File){$fixtureHashes['audio/'+$file.Name]=(Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}}
 $input=[ordered]@{schema='openpilot-media-acceptance-input/v1';nodeids=@($selected|ForEach-Object Name);fixture_sha256=$fixtureHashes;profile=$profile;runner='tests/media_preview.ps1';images=$images;sources=$sources;workers=1;physical_input=$false;video_fixture='synthetic chart, exactly one video stream and zero audio tracks; generator/tool/file hashes recorded';fixture_manifest_sha256=(Get-FileHash (Join-Path $source 'FIXTURES.json')).Hash.ToLowerInvariant();audio_lane=[bool]$Audio;audio_fixture='original A/V and audio files only when -Audio';wsl_fixture='owned regular video and FIFO; WSL stays running'};$json=$input|ConvertTo-Json -Depth 10 -Compress;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
 foreach($module in $modules){$pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDirectory $module));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;$pi.Environment['OPENPILOT_MEDIA_WSL']=$unc;$pi.Environment['OPENPILOT_MEDIA_SCREENSHOT']=Join-Path $ResultDirectory 'media-preview.bmp'
  [void]$pi.ArgumentList.Add($(if($module -eq 'media_audio_tests.exe'){Join-Path $source 'fixtures'}else{$fixtures}));if($module -eq 'media_host_tests.exe'){[void]$pi.ArgumentList.Add($right)}
  $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();$timeout=-not $process.WaitForExit(120000);if($timeout){$process.Kill($true);$process.WaitForExit()};$watch.Stop();$exit=$process.ExitCode;$process.Dispose();$process=$null
  [IO.File]::WriteAllText((Join-Path $ResultDirectory ($module+'-stdout.txt')),$stdout.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory ($module+'-stderr.txt')),$stderr.Result)
  @{schema='openpilot-media-acceptance/v1';module=$module;outcome=$(if($exit -eq 0 -and -not $timeout){'passed'}else{'failed'});exit_code=$exit;timeout=$timeout;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;wsl_stopped=$false;docker_stopped=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory ($module+'-RESULT.json'));Write-Output $stdout.Result;if($exit -or $timeout){throw "Media acceptance failed in $module; evidence retained"}
 }
 $changed=@();foreach($entry in $sources.GetEnumerator()){if((Get-FileHash (Join-Path $root $entry.Key) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value){$changed+=$entry.Key}};if($changed.Count){throw 'Source inputs changed during acceptance'}
 @{schema='openpilot-media-acceptance-closeout/v1';outcome='passed';input_key=$key;modules=$modules;setup_ms=$setupMs;wall_ms=$overall.Elapsed.TotalMilliseconds;audio=$(if($Audio){'ran'}else{'not requested for this scoped video/control run; prior audio acceptance retained'});wsl_stopped=$false;docker_stopped=$false;physical_input=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
}finally{
 if($process){if(-not $process.HasExited){$process.Kill($true)};$process.Dispose()}
 if($created){$cleanup=@'
import os,sys
p=sys.argv[1];assert p.startswith('/tmp/openpilot-media-') and len(p.rsplit('-',1)[1])==32
assert not os.path.islink(p) and open(p+'/.owner').read()=='openpilot-media/v1'
assert set(os.listdir(p))=={'.owner','h264_aac.mp4','not-a-file.mp4'}
for name in ['h264_aac.mp4','not-a-file.mp4','.owner']:os.unlink(p+'/'+name)
os.rmdir(p)
'@
 try{Invoke-OwnedMediaWsl @('--exec','python3','-c',$cleanup,$linux)|Out-Null}catch{Write-Warning 'Owned WSL fixture retained; cleanup could not be confirmed'}}
}
