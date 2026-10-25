<# Branding acceptance: actual PE resources, private-desktop UI and owned shortcut properties. #>
param([string]$BuildDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent;$overall=[Diagnostics.Stopwatch]::StartNew()
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build\branding\'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a fresh evidence directory'};New-Item -ItemType Directory $ResultDirectory|Out-Null
$fixture=Join-Path $ResultDirectory 'fixtures';$appdata=Join-Path $fixture 'appdata';$legacy=Join-Path $appdata 'OpenPilot';New-Item -ItemType Directory $fixture,$legacy|Out-Null
[IO.File]::WriteAllText((Join-Path $fixture 'notes.txt'),'Owned Folderwing branding fixture')
$ini="[functions]`nwsl.helper=off`nsvc.shell=off`nsvc.icons=off`npreview.pdf=off`npreview.image=off`npreview.text=off`npreview.media=off`npreview.archive=off`n"
[IO.File]::WriteAllText((Join-Path $legacy 'openpilot.ini'),$ini);[IO.File]::WriteAllText((Join-Path $legacy 'session.ini'),'; existing owned session')
foreach($file in @('session.ini.workspaces','folder-views.txt','pdf-positions.txt')){[IO.File]::WriteAllText((Join-Path $legacy $file),'owned legacy '+$file)}
$before=(Get-FileHash (Join-Path $legacy 'openpilot.ini')).Hash
$registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json;$sources=[ordered]@{};$nodeids=@()
foreach($s in $registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/branding_host_tests.c::')}){$nodeids+=$s.Name;foreach($file in $s.Value.sources){$sources[$file]=(Get-FileHash (Join-Path $root $file) -Algorithm SHA256).Hash.ToLowerInvariant()}}
if($nodeids.Count -ne 6){throw 'Branding selectors must be impact-bound'}
$images=[ordered]@{};foreach($name in @('folderwing.exe','branding_host_tests.exe')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
$os=Get-CimInstance Win32_OperatingSystem;$profile=[ordered]@{workers=1;os=[Environment]::OSVersion.VersionString;pwsh=$PSVersionTable.PSVersion.ToString();free_memory_kib=$os.FreePhysicalMemory;cpu_load=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage);cache='uncontrolled development host';network='none';containers='unchanged; unused';external_services='none'};$setupMs=$overall.Elapsed.TotalMilliseconds
$input=[ordered]@{schema='folderwing-branding-input/v1';nodeids=$nodeids;sources=$sources;images=$images;profile=$profile;runner='tests/branding.ps1';workers=1;physical_input=$false;private_desktop=$true;fixture='private AppData and session, actual candidate resource/version data, owned modal dialog and shortcut files';full_suite=$false}
$json=$input|ConvertTo-Json -Depth 10 -Compress;[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json);$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant()
$pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDirectory 'branding_host_tests.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;$pi.Environment['APPDATA']=$appdata;foreach($a in @((Join-Path $BuildDirectory 'folderwing.exe'),$fixture)){[void]$pi.ArgumentList.Add($a)}
$watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$o=$process.StandardOutput.ReadToEndAsync();$e=$process.StandardError.ReadToEndAsync();$timeout=-not $process.WaitForExit(15000);if($timeout){$process.Kill($true);$process.WaitForExit()};$watch.Stop();$code=$(if($timeout){124}else{$process.ExitCode});$process.Dispose()
[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stdout.txt'),$o.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stderr.txt'),$e.Result)
@{outcome=$(if($code){'failed'}else{'passed'});exit_code=$code;timed_out=$timeout;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;setup_ms=$setupMs;full_suite=$false;physical_input=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'RESULT.json');Write-Output $o.Result;if($code){throw 'Branding host acceptance failed; evidence retained'}
if((Get-FileHash (Join-Path $legacy 'openpilot.ini')).Hash -ne $before){throw 'Existing settings bytes changed'}
$shortcutDir=Join-Path $fixture 'shortcuts';New-Item -ItemType Directory $shortcutDir|Out-Null;$old=Join-Path $shortcutDir 'OpenPilot.lnk';$shell=New-Object -ComObject WScript.Shell;$link=$shell.CreateShortcut($old);$link.TargetPath=Join-Path $BuildDirectory 'fpdemo.exe';$link.Arguments='--settings= --session=';$link.WorkingDirectory=$fixture;$link.Save()
$backup=Join-Path $fixture 'shortcut-backup';& (Join-Path $root 'tools\folderwing-shortcut.ps1') -BuildDirectory $BuildDirectory -Directory $shortcutDir -BackupDirectory $backup|Set-Content (Join-Path $ResultDirectory 'SHORTCUT.json')
$installed=Join-Path $shortcutDir 'Folderwing.lnk';$link=$shell.CreateShortcut($installed);if($link.Arguments -ne '--settings= --session=' -or $link.WorkingDirectory -ne $fixture -or (Test-Path $old) -or -not(Test-Path (Join-Path $backup 'OpenPilot.lnk')) -or [FolderwingShortcutProperties]::Get($installed) -ne 'Folderwing.FileManager'){throw 'Shortcut migration/property check failed'}
$link.TargetPath=Join-Path $env:SystemRoot 'explorer.exe';$link.Save();$foreign=(Get-FileHash $installed).Hash;$refused=$false
try{& (Join-Path $root 'tools\folderwing-shortcut.ps1') -BuildDirectory $BuildDirectory -Directory $shortcutDir -BackupDirectory (Join-Path $fixture 'refused-backup')|Out-Null}catch{if($_.Exception.Message -like '*targets another app/build*'){$refused=$true}else{throw}}
if(-not $refused -or (Get-FileHash $installed).Hash -ne $foreign){throw 'Foreign shortcut was not preserved'}
@{outcome='passed';input_key=$key;shortcut_arguments_preserved=$true;working_directory_preserved=$true;app_id_verified=$true;foreign_target_refused=$true;legacy_settings_unchanged=$true;wall_ms=$overall.Elapsed.TotalMilliseconds;user_shortcuts_changed=$false;wsl_stopped=$false;docker_stopped=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'CLOSEOUT.json')
Write-Output 'Shortcut properties, backup, preserved arguments and foreign-target refusal passed.'
