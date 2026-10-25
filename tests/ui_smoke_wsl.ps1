# Owned, bounded WSL fixture commands for ui_smoke; dot-sourcing has no effects.
function Invoke-SmokeWsl([ValidateSet('setup','mutation','cleanup')][string]$Phase){
    if($wslOwner -cnotmatch '^[a-f0-9]{32}$' -or ($Phase -ne 'setup' -and $wslRoot -cnotmatch '^/tmp/openpilot-ui-[a-zA-Z0-9]{8}$')){throw 'Exact owned WSL fixture token/root required before command creation'}
    $scriptText=switch($Phase){
        setup {'r=$(mktemp -d /tmp/openpilot-ui-XXXXXXXX) || exit; printf "%s" "$1" > "$r/.openpilot-owner" || exit; printf "%s\n%s\n" "$WSL_DISTRO_NAME" "$r"; mkdir "$r/real" && touch "$r/real/inside" && ln -s real "$r/lnk"'}
        mutation {'r=$1; case "$r" in /tmp/openpilot-ui-????????) ;; *) exit 73;; esac; test -d "$r" && test ! -L "$r" && test "$(cat "$r/.openpilot-owner")" = "$2" || exit 73; mkdir "$r/born"'}
        cleanup {'r=$1; case "$r" in /tmp/openpilot-ui-????????) ;; *) exit 73;; esac; test -d "$r" && test ! -L "$r" && test "$(cat "$r/.openpilot-owner")" = "$2" || exit 73; rm -rf --one-file-system -- "$r"'}
    }
    $si=[Diagnostics.ProcessStartInfo]::new((Get-Command wsl.exe -ErrorAction Stop).Source);$si.UseShellExecute=$false;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
    $arguments=@('--exec','sh','-c',$scriptText,'openpilot-ui')+$(if($Phase -eq 'setup'){@($wslOwner)}else{@($wslRoot,$wslOwner)})
    foreach($arg in $arguments){$si.ArgumentList.Add($arg)}
    $call=[Diagnostics.Stopwatch]::StartNew();$native=[Diagnostics.Process]::Start($si);$nativeBirth=$native.StartTime.ToUniversalTime().Ticks
    $stdout=$native.StandardOutput.ReadToEndAsync();$stderr=$native.StandardError.ReadToEndAsync()
    $finished=$native.WaitForExit(30000)
    if(-not $finished){if($native.StartTime.ToUniversalTime().Ticks -ne $nativeBirth){throw 'Owned WSL command birth changed'};$native.Kill();$native.WaitForExit()}
    $output=$stdout.GetAwaiter().GetResult();$errorText=$stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $out "wsl-$Phase.stdout.log"),$output);[IO.File]::WriteAllText((Join-Path $out "wsl-$Phase.stderr.log"),$errorText)
    $facts=@{phase=$Phase;exit_code=$native.ExitCode;finished=$finished;pid=$native.Id;birth_ticks=$nativeBirth;wall_ms=$call.ElapsedMilliseconds;owner=$wslOwner;root=$wslRoot;output=$output;error=$errorText}
    $facts|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out "wsl-$Phase.json")
    [pscustomobject]$facts
}
