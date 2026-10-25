<# Per-folder choices: actual navigation, sorting, two panes, restarts and WSL identity. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build\folderviews';New-Item -ItemType Directory -Force $out|Out-Null
$exe=Join-Path $root "build\$Config\folderwing.exe"
$script:passed=0;$script:failed=0
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Caption($h){$s=[Text.StringBuilder]::new(512);[FpUi]::GetWindowTextW($h,$s,512)|Out-Null;$s.ToString()}
function Sort-Key($h=$script:pane0){[FpUi]::GetPropW($h,'FolderView.Sort').ToInt64()-1}
function Descending($h=$script:pane0){[FpUi]::GetPropW($h,'FolderView.Descending').ToInt64()-1}
function Tile-Size($h=$script:pane0){[FpUi]::GetPropW($h,'FolderView.Size').ToInt64()}
function Navigate([string]$path,$pane=$script:pane0){
    Chord $pane 'Ctrl' 0x4C 20;$edit=(Pane-Edits $pane)[0];Send-Text $edit $path;Key $edit 0x0D 20
    if(-not (Wait-Until {(Window-Text $edit).TrimEnd('\') -ceq $path.TrimEnd('\')} 5000)){throw "Navigation failed: $path"}
}
function Start-ReadyDemo([string[]]$arguments){
    $process=Start-Demo $exe $arguments
    if(-not (Wait-Until {[FpUi]::GetPropW($script:main,'Settings.Watching').ToInt64() -eq 1} 3000)){
        Stop-Demo $process|Out-Null;throw 'The private settings watcher did not start'
    }
    $process
}
function Reset-Folder($pane=$script:pane0){
    $r=[FpUi+RECT]::new();[FpUi]::GetWindowRect($pane,[ref]$r)|Out-Null
    Post $pane $WM_CONTEXTMENU $pane ((($r.B-45) -shl 16) -bor ($r.L+40))
    $menu=Open-Menu
    if(-not $menu){return $false}
    if($menu.Items -notcontains "Reset this folder's view"){Key $menu.Window 0x1B 20;return $false}
    Pick-Menu $menu "Reset this folder's view"
}
$scratch=Join-Path $env:TEMP "openpilot_folderviews_$PID";$p=$null;$linux=$null
try {
    $gallery="$scratch\Pictures β";$code="$scratch\Code";$other="$scratch\Unvisited"
    New-Item -ItemType Directory -Force $gallery,$code,$other|Out-Null
    foreach($folder in @($gallery,$code,$other)){
        Set-Content -LiteralPath "$folder\a_small.dat" -Value 'a' -NoNewline;Set-Content -LiteralPath "$folder\z_large.dat" -Value ('z'*80) -NoNewline;Set-Content -LiteralPath "$folder\m_middle.dat" -Value ('m'*8) -NoNewline
    }
    Check 'the file-order fixture contains all three files' (@(Get-ChildItem -LiteralPath $gallery -File).Count -eq 3)
    $settings="$scratch\settings.ini";$session="$scratch\session.ini";$memory="$scratch\folder-views.txt"
    $base="[functions]`npreview.image = off`npreview.pdf = off`npreview.text = off`n[panel.side]`nslide = off`n"
    Set-Content $settings $base
    $p=Start-ReadyDemo @("--settings=$settings","--session=$session","--profile-out=$out\folder-profile.csv",$gallery,$code)
    Check 'a new folder starts with the default list and name sort' ((Caption $pane0) -eq 'Details' -and (Sort-Key) -eq 0 -and (Descending) -eq 0 -and (Tile-Size) -eq 160)
    Check 'a fresh session creates no view file merely by navigating' (-not (Test-Path $memory))
    Chord $pane0 @('Ctrl','Alt') 0x54 20
    Check 'the layout shortcut switches the picture folder to tiles' ((Caption $pane0) -match 'Thumbnails 160') (Caption $pane0)
    Chord $pane0 @('Ctrl','Shift') 0x26 20
    Check 'the size shortcut makes the picture tiles large' ((Caption $pane0) -match 'Thumbnails 256' -and (Tile-Size) -eq 256) "$(Caption $pane0); size=$(Tile-Size)"
    Chord $pane0 'Ctrl' 0x33 20
    Check 'the picture folder accepts large tiles and size order' ((Caption $pane0) -match 'Thumbnails 256' -and (Sort-Key) -eq 2 -and (Descending) -eq 1) "$(Caption $pane0); sort=$(Sort-Key); descending=$(Descending); size=$(Tile-Size)"
    Key $pane0 0x24 20
    Check 'the remembered sort actually orders the files largest first' ((Caption $pane0) -match 'Cursor z_large.dat') (Caption $pane0)
    Navigate $code
    Check 'an unconfigured code folder uses its own defaults' ((Caption $pane0) -eq 'Details' -and (Sort-Key) -eq 0 -and (Tile-Size) -eq 160)
    Chord $pane0 'Ctrl' 0x31 20
    Check 'the code folder can reverse its name sort independently' ((Sort-Key) -eq 0 -and (Descending) -eq 1)
    Navigate $gallery
    Check 'returning restores the picture folder choices' ((Caption $pane0) -match 'Thumbnails 256' -and (Sort-Key) -eq 2 -and (Descending) -eq 1)
    Post $pane0 $WM_SYSKEYDOWN 0x25 0x20000000;Start-Sleep -Milliseconds 200
    Check 'Back restores the code folder view' ((Caption $pane0) -eq 'Details' -and (Descending) -eq 1)
    Post $pane0 $WM_SYSKEYDOWN 0x27 0x20000000;Start-Sleep -Milliseconds 200
    Check 'Forward restores the picture folder view' ((Caption $pane0) -match 'Thumbnails 256' -and (Sort-Key) -eq 2)
    Chord $pane0 'Ctrl' 0x54 20;Navigate $code;Chord $pane0 @('Ctrl','Shift') 0x09 20
    Check 'switching tabs restores the shown folder choices' ((Caption $pane0) -match 'Thumbnails 256' -and (Sort-Key) -eq 2)
    Navigate $gallery $pane1
    Check 'the other pane restores the same folder choices' ((Caption $pane1) -match 'Thumbnails 256' -and (Sort-Key $pane1) -eq 2)
    Chord $pane1 @('Ctrl','Alt') 0x54 20
    Check 'a layout change updates both panes showing the same folder' ((Caption $pane0) -eq 'Details' -and (Caption $pane1) -eq 'Details')
    Chord $pane0 @('Ctrl','Alt') 0x54 20
    Set-Content $settings ($base.Replace('preview.text = off',"preview.text = off`nview.thumbnails = off"))
    Check 'disabling thumbnail support temporarily uses lists' (Wait-Until {(Caption $pane0) -eq 'Details' -and (Caption $pane1) -eq 'Details'} 5000)
    Set-Content $settings $base
    Check 'reenabling thumbnails retains the saved folder choice' (Wait-Until {(Caption $pane0) -match 'Thumbnails 256' -and (Caption $pane1) -match 'Thumbnails 256'} 5000)
    Shot $out '01_remembered'
    Check 'the first window closes cleanly' (Stop-Demo $p);$p=$null
    Check 'closing atomically writes a separate view file' (Test-Path $memory)
    $saved=Get-Content $memory -Raw
    Check 'the view file keeps Unicode paths and explicit choices' ($saved.Contains($gallery) -and $saved -match '7\s+1\s+256\s+2\s+1')
    Check 'no temporary files remain after a successful save' (@(Get-ChildItem $scratch -Filter 'folder-views.txt.*.tmp').Count -eq 0)
    # Command-line folders and session.restore=off must not disable folder memory.
    Set-Content $settings ($base.Replace('preview.text = off',"preview.text = off`nsession.restore = off"))
    $p=Start-ReadyDemo @("--settings=$settings","--session=$session",$gallery,$code)
    Check 'a restart restores picture preferences with tab restoration off' ((Caption $pane0) -match 'Thumbnails 256' -and (Sort-Key) -eq 2)
    Check 'a restart restores the code sort separately' ((Caption $pane1) -eq 'Details' -and (Descending $pane1) -eq 1)
    Set-Content $settings ($base+"[view.thumbnails]`nmode = thumbnails`nsize = small`n")
    Check 'explicit picture choices survive changed global defaults' (Wait-Until {(Caption $pane0) -match 'Thumbnails 256' -and (Sort-Key) -eq 2} 5000)
    Check 'unset code layout and size follow the new defaults' (Wait-Until {(Caption $pane1) -match 'Thumbnails 96' -and (Descending $pane1) -eq 1} 5000)
    Check 'the reset command is available in the folder menu' (Reset-Folder)
    Check 'reset applies current defaults and ascending name order' (Wait-Until {(Caption $pane0) -match 'Thumbnails 96' -and (Sort-Key) -eq 0 -and (Descending) -eq 0} 3000)
    Navigate $other;Navigate $gallery
    Check 'the reset survives leaving and returning' ((Caption $pane0) -match 'Thumbnails 96' -and (Sort-Key) -eq 0)
    Check 'reset leaves the other folder sort alone' ((Descending $pane1) -eq 1)
    Check 'the reset window closes cleanly' (Stop-Demo $p);$p=$null
    Check 'reset removes the picture override from disk' (-not (Get-Content $memory -Raw).Contains($gallery))
    $p=Start-ReadyDemo @("--settings=$settings","--session=$session",$gallery,$code)
    Check 'reset remains in effect after a restart' ((Caption $pane0) -match 'Thumbnails 96' -and (Sort-Key) -eq 0)
    # Disable memory: it retains the established pane behavior and writes no new preferences.
    Set-Content $settings ($base.Replace('preview.text = off',"preview.text = off`nview.folder_memory = off"))
    Start-Sleep -Milliseconds 500
    Chord $pane0 'Ctrl' 0x34 20;Navigate $other
    Check 'disabled folder memory keeps the pane sort across navigation' ((Sort-Key) -eq 3)
    Check 'disabled folder memory closes cleanly' (Stop-Demo $p);$p=$null
    Check 'disabled memory creates no override for the unvisited folder' (-not (Get-Content $memory -Raw).Contains($other))
    # Linux suffixes keep case and significant trailing spaces; server aliases share preferences.
    Set-Content $settings $base
    $dist=@(& wsl.exe -l -q|ForEach-Object {$_.Replace([string][char]0,'').Trim()}|Where-Object {$_})[0]
    if(-not $dist){throw 'WSL acceptance requires an available distro'}
    $linux="/tmp/openpilot-folderviews-$PID"
    & wsl.exe -d $dist -- mkdir -p "$linux/A" "$linux/a" "$linux/space β "
    $upper="\\wsl.localhost\$dist$($linux.Replace('/','\'))\A";$lower="\\wsl.localhost\$dist$($linux.Replace('/','\'))\a"
    $space="\\wsl.localhost\$dist$($linux.Replace('/','\'))\space β "
    $p=Start-ReadyDemo @("--settings=$settings","--session=$session","--profile-out=$out\wsl-folder-profile.csv",$gallery,$code)
    Navigate $upper;Chord $pane0 @('Ctrl','Alt') 0x54 20;Chord $pane0 @('Ctrl','Shift') 0x28 20;Chord $pane0 'Ctrl' 0x34 20
    Navigate $lower;Chord $pane0 'Ctrl' 0x31 20
    Check 'case-distinct Linux folders start with independent choices' ((Caption $pane0) -eq 'Details' -and (Sort-Key) -eq 0 -and (Descending) -eq 1)
    Navigate $upper
    Check 'the uppercase Linux folder restores its own tiles and date sort' ((Caption $pane0) -match 'Thumbnails 96' -and (Sort-Key) -eq 3)
    $alias=$upper.Replace('wsl.localhost','wsl$');Navigate $alias $pane1
    Check 'the wsl$ alias restores the same folder preference' ((Caption $pane1) -match 'Thumbnails 96' -and (Sort-Key $pane1) -eq 3)
    Chord $pane1 @('Ctrl','Alt') 0x54 20
    Check 'a change synchronizes panes using different WSL server aliases' ((Caption $pane0) -eq 'Details' -and (Caption $pane1) -eq 'Details')
    Chord $pane0 @('Ctrl','Alt') 0x54 20
    Navigate $space;Chord $pane0 'Ctrl' 0x32 20;Navigate $lower;Navigate $space
    Check 'Unicode and trailing-space Linux paths retain their exact choices' ((Sort-Key) -eq 1 -and (Caption $pane0) -eq 'Details')
    Shot $out '02_wsl'
    Check 'Linux preferences close cleanly' (Stop-Demo $p);$p=$null
    $p=Start-ReadyDemo @("--settings=$settings","--session=$session",$upper,$lower)
    Check 'Linux preferences restore after restart without changing case identity' ((Caption $pane0) -match 'Thumbnails 96' -and (Sort-Key) -eq 3 -and (Caption $pane1) -eq 'Details' -and (Descending $pane1) -eq 1)
    Check 'the final explorer closes cleanly' (Stop-Demo $p);$p=$null
    foreach($report in @('folder-profile.csv','wsl-folder-profile.csv')){
        $profile=Import-Csv "$out\$report"
        Check "$report has no function lifetime violations" (($profile|Where-Object name -eq 'fn_violations').value -eq '0')
        $ui=$profile|Where-Object {$_.kind -eq 'zone' -and $_.name -eq 'ui.dispatch'}
        Check "$report keeps UI dispatch below 16 ms" ([double]$ui.max_ms -lt 16) $ui.max_ms
    }
}finally{
    if($p -and -not $p.HasExited){Stop-Demo $p|Out-Null}
    if($linux -and $dist){& wsl.exe -d $dist -- rm -rf $linux}
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
"`n$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
