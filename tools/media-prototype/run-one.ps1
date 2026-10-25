param([ValidateSet('mf','libmpv')][string]$Engine,[string]$File,[string]$Result,[string]$Bmp,[int]$ObserveMs=500,[switch]$Visible,[switch]$AudioOnly)
$ErrorActionPreference='Stop';$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent;$candidate=Join-Path $root 'build\media_prototype\20261023\candidate\media-prototype.exe'
if(Test-Path -LiteralPath $Result){throw 'Preserve prior prototype evidence; use a new output path'}
$pi=[Diagnostics.ProcessStartInfo]::new($candidate);$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
foreach($arg in @('--file',$File,'--report',$Result,'--observe-ms',"$ObserveMs")){[void]$pi.ArgumentList.Add($arg)}
if($AudioOnly){[void]$pi.ArgumentList.Add('--audio-only')};if($Engine -eq 'libmpv'){[void]$pi.ArgumentList.Add('--mpv')};if($Visible){[void]$pi.ArgumentList.Add('--visible')};if($Bmp){[void]$pi.ArgumentList.Add('--bmp');[void]$pi.ArgumentList.Add($Bmp)}
$start=[Diagnostics.Stopwatch]::GetTimestamp();[void]$pi.ArgumentList.Add('--parent-qpc');[void]$pi.ArgumentList.Add("$start")
$p=[Diagnostics.Process]::Start($pi);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();$timed=$false
if(-not $p.WaitForExit(35000)){$timed=$true;$p.Kill($true);$p.WaitForExit()};$end=[Diagnostics.Stopwatch]::GetTimestamp();$exit=$p.ExitCode;$p.Dispose()
[IO.File]::WriteAllText($Result+'.stdout.txt',$stdout.Result);[IO.File]::WriteAllText($Result+'.stderr.txt',$stderr.Result)
@{engine=$Engine;file=$File;exit_code=$exit;timeout=$timed;process_wall_ms=1000.0*($end-$start)/[Diagnostics.Stopwatch]::Frequency;physical_input=$false;audio_muted=$true;wsl_stopped=$false}|ConvertTo-Json|Set-Content ($Result+'.process.json')
if(Test-Path $Result){Get-Content $Result -Raw}else{Write-Output "No report: exit $exit timeout $timed"};if($exit){Write-Output $stderr.Result}
