# Read-only probe. Explicit ProcessStartInfo avoids PATHEXT/document association.
function Read-MediaAudioEndpoint([string]$Destination) {
 $root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
 $info=[Diagnostics.ProcessStartInfo]::new((Join-Path $root 'build/media_prototype/20261023/candidate/audio-probe.exe'));$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true
 $process=[Diagnostics.Process]::Start($info);$output=$process.StandardOutput.ReadToEndAsync()
 if(-not $process.WaitForExit(5000)){$process.Kill($true);$process.Dispose();throw 'Audio endpoint probe timed out'}
 $code=$process.ExitCode;$process.Dispose();[IO.File]::WriteAllText($Destination,$output.Result)
 return $code
}
