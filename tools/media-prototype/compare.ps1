param([string]$ResultDirectory,[int]$Repetitions=3)
$ErrorActionPreference='Stop';$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent;$cohort=Join-Path $root 'build\media_prototype\20261023'
if(-not $ResultDirectory){$ResultDirectory=Join-Path $cohort 'comparison-1'}
if(Test-Path $ResultDirectory){throw 'Use a fresh comparison directory'};New-Item -ItemType Directory $ResultDirectory|Out-Null
Add-Type @'
using System;using System.Runtime.InteropServices;
public static class MediaPrototypeHost {
 [DllImport("kernel32.dll")]static extern bool GetSystemTimes(out ulong idle,out ulong kernel,out ulong user);
 [StructLayout(LayoutKind.Sequential)]class Memory{public uint length=64,load;public ulong physical,available,pagefile,availablePagefile,virtualSize,availableVirtual,extended;}
 [DllImport("kernel32.dll")]static extern bool GlobalMemoryStatusEx(Memory m);
 public static ulong[] Times(){ulong i,k,u;GetSystemTimes(out i,out k,out u);return new ulong[]{i,k,u};}
 public static ulong[] Ram(){var m=new Memory();GlobalMemoryStatusEx(m);return new ulong[]{m.physical,m.available};}
}
'@
$cases=@(
 @{name='h264_aac.mp4';audio=$false},@{name='h264_aac.mkv';audio=$false},@{name='hevc_aac.mp4';audio=$false},@{name='av1_aac.mp4';audio=$false},@{name='vp9_opus.webm';audio=$false},@{name='vp8_vorbis.webm';audio=$false},@{name='mpeg4_pcm.avi';audio=$false},
 @{name='pcm.wav';audio=$true},@{name='mp3.mp3';audio=$true},@{name='aac.m4a';audio=$true},@{name='flac.flac';audio=$true},@{name='vorbis.ogg';audio=$true},@{name='opus.opus';audio=$true},@{name='h264_4k60.mp4';audio=$false},@{name='corrupt.mp4';audio=$false})
$inputs=[ordered]@{};foreach($relative in @('tools/media-prototype/audio-endpoint.ps1','tools/media-prototype/audio-probe.cpp','build/media_prototype/20261023/candidate/audio-probe.exe','tools/media-prototype/player.cpp','tools/media-prototype/build.py','tools/media-prototype/fixtures.py','tools/media-prototype/run-one.ps1','tools/media-prototype/compare.ps1','build/media_prototype/20261023/FIXTURES.json','build/media_prototype/20261023/dependencies/PROVENANCE.json','build/media_prototype/20261023/candidate/media-prototype.exe','build/media_prototype/20261023/candidate/libmpv-2.dll')){$inputs[$relative]=(Get-FileHash (Join-Path $root $relative) -Algorithm SHA256).Hash.ToLowerInvariant()}
foreach($case in $cases){$relative='build/media_prototype/20261023/fixtures/'+$case.name;$inputs[$relative]=(Get-FileHash (Join-Path $root $relative) -Algorithm SHA256).Hash.ToLowerInvariant()}
$profile=[ordered]@{workers=1;ordering='serial, alternating first engine by repetition and case';cache='fresh processes, existing OS/file/driver caches; not machine-cold';output='same D3D11 BGRA texture 640x360, hidden owned windows; first verified readback, no DWM timing';audio='muted WASAPI device, audio-only files skip D3D initialization';observe_ms=1500;repetitions=$Repetitions;physical_input=$false;release_changed=$false;wsl_stopped=$false;docker_stopped=$false}
$input=[ordered]@{schema='openpilot-media-comparison-input/v1';sources_and_binaries=$inputs;cases=$cases;profile=$profile};$json=$input|ConvertTo-Json -Depth 10 -Compress;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
$environment=@{utc=[DateTime]::UtcNow.ToString('o');os=[Environment]::OSVersion.VersionString;logical_processors=[Environment]::ProcessorCount;memory=[MediaPrototypeHost]::Ram();gpu=@(Get-CimInstance Win32_VideoController|Select-Object Name,DriverVersion);cpu=@(Get-CimInstance Win32_Processor|Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);codec_packages=@(Get-AppxPackage|Where-Object {$_.Name -match 'HEVC|AV1Video|VP9Video|WebMedia'}|Select-Object Name,Version)};$environment|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory 'ENVIRONMENT.json')
. (Join-Path $PSScriptRoot 'audio-endpoint.ps1')
$rows=[Collections.Generic.List[object]]::new();$index=0
for($repeat=0;$repeat -lt $Repetitions;$repeat++){
 foreach($case in $cases){if($case.name -eq 'corrupt.mp4' -and $repeat -gt 0){continue};$order=if(($repeat+$index)%2){@('libmpv','mf')}else{@('mf','libmpv')};$index++
  foreach($engine in $order){$id='{0}-{1}-{2}' -f ($repeat+1),$case.name,$engine;$report=Join-Path $ResultDirectory ($id+'.json');$params=@{Engine=$engine;File=(Join-Path $cohort ('fixtures\'+$case.name));Result=$report;ObserveMs=1500};if($case.audio){$params.AudioOnly=$true};if($repeat -eq 0 -and -not $case.audio -and $case.name -ne 'corrupt.mp4'){$params.Bmp=Join-Path $ResultDirectory ($id+'.bmp')}
   $endpointExit=Read-MediaAudioEndpoint (Join-Path $ResultDirectory ($id+'.endpoints.txt'));if($null -eq $endpointExit){throw 'Endpoint probe did not return an exit status'};if($endpointExit){@{completed=$false;outcome='interrupted-environment';reason='default multimedia endpoint unavailable';rows_retained=$rows.Count}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'RESULT.json');throw 'Default audio endpoint unavailable; retained partial evidence'}
   $before=[MediaPrototypeHost]::Times();$memory=[MediaPrototypeHost]::Ram();$output=& (Join-Path $PSScriptRoot 'run-one.ps1') @params;$after=[MediaPrototypeHost]::Times();$total=[double](($after[1]-$before[1])+($after[2]-$before[2]));$hostCpu=if($total -gt 0){100*(1-($after[0]-$before[0])/$total)}else{-1}
   $outer=Get-Content ($report+'.process.json') -Raw|ConvertFrom-Json;$result=if(Test-Path $report){Get-Content $report -Raw|ConvertFrom-Json}else{$null}
   $row=[ordered]@{id=$id;engine=$engine;file=$case.name;audio_only=$case.audio;repeat=$repeat+1;host_cpu_percent=$hostCpu;available_ram_bytes=$memory[1];process=$outer;result=$result};$rows.Add($row);$rows|ConvertTo-Json -Depth 12|Set-Content (Join-Path $ResultDirectory 'ROWS.json')
   Write-Output ('{0}: exit={1}, ready={2}, first={3}ms, private={4}MiB, host CPU={5:N1}%' -f $id,$outer.exit_code,$result.ready,$result.first_verified_render_from_launch_ms,([math]::Round($result.ready_private_bytes/1MB,1)),$hostCpu)
  }
 }
}
$finalEndpointExit=Read-MediaAudioEndpoint (Join-Path $ResultDirectory 'FINAL_ENDPOINTS.txt')
$changed=@();foreach($property in $inputs.GetEnumerator()){if((Get-FileHash (Join-Path $root $property.Key) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $property.Value){$changed+=$property.Key}}
@{schema='openpilot-media-comparison/v1';input_key=$key;completed=$true;runs=$rows.Count;input_changed=$changed;final_endpoint_exit=$finalEndpointExit;profile=$profile;stopped_wsl=$false;stopped_docker=$false}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
if($changed.Count){throw 'Inputs changed during the comparison'}

if($finalEndpointExit){throw 'Audio endpoint unavailable at completion; inspect raw rows'}
