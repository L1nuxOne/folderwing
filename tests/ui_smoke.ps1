<#
.SYNOPSIS
  Drives folderwing.exe with posted messages: dual-pane copy, quick filter,
  navigation, history, tree clicks, the split toggle and debug mode.
  Uses a scratch folder under %TEMP% and removes it afterwards.
  Screenshots land in build/ui_smoke. Run ./build.ps1 first.
  ShellMenuWaitMs defaults to 15 seconds; use an explicit resource profile for
  a diagnosed slow third-party shell handler. Keep its before/after timing evidence.
  SeparateExplorerMenu explicitly defers the three external-menu assertions for
  a diagnosed handler failure; the normal profile keeps running them.
#>
param([string] $Config = 'release', [ValidateRange(1000,60000)] [int] $ShellMenuWaitMs = 15000, [switch] $SeparateExplorerMenu, [string] $EvidenceRoot = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_input.ps1"
. "$PSScriptRoot\ui_readiness.ps1"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\$Config\folderwing.exe"
$out = if ($EvidenceRoot) { $EvidenceRoot } else { Join-Path $root 'build\ui_smoke' }
New-Item -ItemType Directory -Force $out | Out-Null

$script:failed = 0
$script:passed = 0
function Check([string] $what, [bool] $ok, [string] $detail = '') {
    if ($ok) { "  ok    $what"; $script:passed++ } else { "  FAIL  $what $detail"; $script:failed++ }
}
function Close-SmokeMenu($menu) {
    # Custom menus consume queued keyboard messages in their modal loop.
    # A synchronous Key bypasses that loop and does not dismiss the popup.
    [FpUi]::OwnedWindow($menu.Window, $script:demoPid)
    Post $menu.Window $WM_KEYDOWN 0x1B (Key-LParam 0x1B)
    if (-not (Wait-Until { -not [FpUi]::IsWindowVisible($menu.Window) } 300)) {
        throw 'The owned popup did not close within its existing 300 ms interval'
    }
}
function Session-Paths([string]$text,[ValidateSet('tab','pin','folder')][string]$key) {
    $pattern=if($key -eq 'tab'){'(?m)^tab16 = [^ \r\n]+ [01] u16v1:([0-9a-fA-F]+)\r?$'}
             else {"(?m)^${key}16 = u16v1:([0-9a-fA-F]+)\r?$"}
    foreach($match in [regex]::Matches($text,$pattern)) {
        $hex=$match.Groups[1].Value
        $path=[Text.StringBuilder]::new($hex.Length/4)
        for($i=0;$i -lt $hex.Length;$i+=4){$null=$path.Append([char][Convert]::ToInt32($hex.Substring($i,4),16))}
        $path.ToString()
    }
}

# scratch layout: left/{sub/inner.txt, notes.txt}, right/
$scratch = Join-Path $env:TEMP "fpdemo_ui_$PID-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $scratch | Out-Null
$left = Join-Path $scratch 'left'
$right = Join-Path $scratch 'right'
New-Item -ItemType Directory -Force "$left\sub", $right | Out-Null
Set-Content "$left\sub\inner.txt" 'hello'
Set-Content "$left\notes.txt" 'notes'

$script:readinessEvidence=Join-Path $out "readiness-$PID.jsonl"
$p = Start-Demo $exe @("--profile-out=$out\initial-copy-$PID.csv",$left, $right)
try {
    Start-Sleep -Milliseconds 500

    Check 'opens left folder' ((Title) -eq 'left - Folderwing') "(title '$(Title)')"
    Check 'tree and both panes visible' ([FpUi]::IsWindowVisible($tree) -and [FpUi]::IsWindowVisible($pane1))
    Shot $out '1_dual'

    # F5 copies the cursor item ("sub", folders first) into the right pane's folder
    $copyClock = [Diagnostics.Stopwatch]::StartNew()
    if (-not (Wait-Until {
        [FpUi]::GetPropW($pane0, 'Address.Loading') -eq [IntPtr]::Zero -and
        [FpUi]::GetPropW($pane1, 'Address.Loading') -eq [IntPtr]::Zero -and
        [FpUi]::GetPropW($pane0, 'Address.Error') -eq [IntPtr]::Zero
    } (Input-Remaining $copyClock 3000))) { throw 'The copy fixture did not finish loading' }
    Send-AcknowledgedKey $pane0 0x74 (Input-Remaining $copyClock 3000)
    $copyReady = Wait-Until {
        $script:opSide = [FpUi]::FindWindowExW($main, [IntPtr]::Zero, 'OpenPilotSide', [IntPtr]::Zero)
        $script:opSlot = [FpUi]::FindWindowExW($opSide, [IntPtr]::Zero, 'OpenPilotSideSlot', [IntPtr]::Zero)
        $opSlot -ne [IntPtr]::Zero -and
        @(Get-Process folderwing-operations -ErrorAction SilentlyContinue | Where-Object { $_.Parent.Id -eq $script:demoPid }).Count -eq 1
    } (Input-Remaining $copyClock 3000)
    Check 'F5 asks in Operations before copying' $copyReady
    if (-not $copyReady) { Write-UiReadinessFailure 'A04 first F5 admission';throw 'The owned Operations input target was not ready; request/selection/dispatch remain unproved unless phase facts establish them' }
    Start-Sleep -Milliseconds 250
    Key $opSlot 0x0D
    Chord $opSlot @('Ctrl','Shift') 0x4F # hide Operations; the copy keeps running
    Check 'F5 copied sub/ to the right pane' (Wait-Until { Test-Path "$right\sub\inner.txt" })
    Start-Sleep -Milliseconds 400
    Shot $out '2_after_copy'

    # quick filter + enter, then Backspace back up
    Send-Text $pane0 'su'
    Key $pane0 0x0D
    Check 'filter + Enter opens sub' (Wait-Until { (Title) -eq 'sub - Folderwing' }) "(title '$(Title)')"
    Key $pane0 0x08
    Check 'Backspace goes to parent' (Wait-Until { (Title) -eq 'left - Folderwing' }) "(title '$(Title)')"
    Post $pane0 $WM_SYSKEYDOWN 0x25 0x20000000; Start-Sleep -Milliseconds 200   # Alt+Left
    Check 'Alt+Left goes back' (Wait-Until { (Title) -eq 'sub - Folderwing' }) "(title '$(Title)')"

    # clicking the first tree root (the user's home) navigates the active pane
    Key $tree 0x24 # Home: select and reveal the first tree root
    Click $tree ([int](80 * $scale)) ([int]((6 + 12) * $scale))
    $homeName = Split-Path $env:USERPROFILE -Leaf
    Check 'tree click navigates active pane' (Wait-Until { (Title) -eq "$homeName - Folderwing" }) "(title '$(Title)')"
    Shot $out '3_tree_home'

    # Ctrl+\ toggles the second pane
    Post $pane0 $WM_CHAR 0x1C; Start-Sleep -Milliseconds 200
    Check 'Ctrl+\ hides the right pane' (-not [FpUi]::IsWindowVisible($pane1))
    Shot $out '4_single'
    Post $pane0 $WM_CHAR 0x1C; Start-Sleep -Milliseconds 200
    Check 'Ctrl+\ shows it again' ([FpUi]::IsWindowVisible($pane1))

    # F12 opens the debug panel (and starts profiling), F12 again closes it
    Check 'debug panel hidden by default' (-not [FpUi]::IsWindowVisible($dbg))
    $p.Refresh()
    '  memory: working set {0:N1} MB, private {1:N1} MB' -f ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB)
    Key $pane0 0x7B
    Check 'F12 shows the debug panel' ([FpUi]::IsWindowVisible($dbg))
    Key $pane0 0x08
    Start-Sleep -Milliseconds 700
    Shot $out '5_debug_panel'
    Key $pane0 0x7B
    Check 'F12 hides it again' (-not [FpUi]::IsWindowVisible($dbg))
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}

# shell integration: rename, new folder, context menu, properties, drop targets
$p = Start-Demo $exe @($left, $right)
try {
    Start-Sleep -Milliseconds 600
    Check 'panes accept drops' ([FpUi]::GetPropW($pane0, 'OleDropTargetInterface') -ne [IntPtr]::Zero)
    Check 'tree accepts drops' ([FpUi]::GetPropW($tree, 'OleDropTargetInterface') -ne [IntPtr]::Zero)
    Shot $out '6_icons'

    # rows: sub/, notes.txt. F2 on notes.txt selects "notes" only, so typing keeps ".txt"
    Key $pane0 0x28
    Key $pane0 0x71
    Check 'F2 opens a rename box' (Wait-Until { (Pane-Edits $pane0).Count -eq 2 })
    $edits = Pane-Edits $pane0
    if ($edits.Count -eq 2) {
        Send-Text $edits[1] 'renamed'
        Key $edits[1] 0x0D
        Check 'rename keeps the extension' (Wait-Until { (Test-Path "$left\renamed.txt") -and -not (Test-Path "$left\notes.txt") })
    }

    # F7 creates "New folder" and opens its rename box
    Key $pane0 0x76
    Check 'F7 creates a folder' (Wait-Until { Test-Path "$left\New folder" })
    Check 'the new folder is in rename mode' (Wait-Until { (Pane-Edits $pane0).Count -eq 2 })
    $edits = Pane-Edits $pane0
    if ($edits.Count -eq 2) {
        Send-Text $edits[1] 'made'
        Key $edits[1] 0x0D
        Check 'new folder renamed while created' (Wait-Until { (Test-Path "$left\made") -and -not (Test-Path "$left\New folder") })
    }
    Start-Sleep -Milliseconds 300

    # menu key on the selected folder: OpenPilot's menu, Explorer's behind
    # "More from Windows…" (it loads every installed shell extension: allow seconds)
    $more = "More from Windows$([char]0x2026)"
    Post $pane0 $WM_CONTEXTMENU $pane0 -1
    $menu = Open-Menu 3000 'OpenPilotMenu'
    Check 'context menu opens, drawn by the app' ($null -ne $menu)
    if(-not $menu){throw 'Dependent shell inputs stopped: initial app menu was not observed'}
    if ($menu) {
        Check 'menu has app items' ($menu.Items -contains 'Open in other pane' -and $menu.Items -contains 'Copy path' -and
                                    $menu.Items -contains 'Rename' -and $menu.Items -contains 'Properties') "(items: $($menu.Items -join ', '))"
        Check 'Explorer''s items are one item away' ($menu.Items[-1] -eq $more)
        if ($SeparateExplorerMenu) {
            '  SKIP  More from Windows can be picked (external handler finding, separate scope)'
            '  SKIP  More from Windows shows Explorer menu (external handler finding, separate scope)'
            '  SKIP  Esc closes Explorer menu (external handler finding, separate scope)'
            Close-SmokeMenu $menu
        } else {
            $pickedMore=Pick-Menu $menu $more
            Check 'More from Windows can be picked' $pickedMore
            if(-not $pickedMore){throw 'Dependent shell inputs stopped: More from Windows was not picked'}
            $shellMenu = Open-Menu $ShellMenuWaitMs '#32768'
            Check 'More from Windows shows Explorer''s menu' ($null -ne $shellMenu -and $shellMenu.Items -contains 'Properties' -and
                                                           -not ($shellMenu.Items -contains 'Copy path')) "(items: $($shellMenu.Items -join ', '))"
            if(-not $shellMenu -or -not($shellMenu.Items -contains 'Properties') -or $shellMenu.Items -contains 'Copy path'){throw 'Dependent shell inputs stopped: native menu was not observed; no Escape, next-menu or Properties action sent'}
            Close-SmokeMenu $shellMenu
            Check 'Esc closes Explorer''s menu' (Wait-Until { (Find-DemoMenu) -eq [IntPtr]::Zero } 2000)
        }
    }
    Post $pane0 $WM_CONTEXTMENU $pane0 -1
    $menu = Open-Menu 3000 'OpenPilotMenu'
    if(-not $menu){throw 'Dependent shell inputs stopped: menu for outside dismissal was not observed'}
    if ($menu) {
        Click $menu.Window -20 -20
        $outsideClosed=Wait-Until { (Find-DemoMenu) -eq [IntPtr]::Zero } 2000
        Check 'a click outside closes the menu' $outsideClosed
        if(-not $outsideClosed){throw 'Dependent shell inputs stopped: outside dismissal did not close the observed menu'}
    }
    Post $pane0 $WM_CONTEXTMENU $pane0 -1
    $menu = Open-Menu 3000 'OpenPilotMenu'
    Check 'the menu opens again' ($null -ne $menu)
    if(-not $menu){throw 'Dependent shell inputs stopped: next app menu was not observed'}
    if ($menu) {
        Check 'menu item runs' (Pick-Menu $menu 'Open in other pane')
        Check '"Open in other pane" navigates the right pane' (Wait-Until { (Window-Text (Pane-Edits $pane1)[0]) -eq "$left\made" }) "(right pane: '$(Window-Text (Pane-Edits $pane1)[0])')"
    }

    # The intended item and ordinary OS key submission are prerequisites to
    # dialog discovery; an unsubmitted command is never a Properties failure.
    . "$PSScriptRoot\shell_ui.ps1" -Config $Config -DefinitionsOnly
    $script:shellPhaseFile=Join-Path $out "shell-phases-$PID.jsonl"
    Invoke-ShellPropertiesCheck 'made Properties' "$left\made"

} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}

# tabs, breadcrumbs, pins and the session file (Ctrl+letter keys arrive as
# control characters: posted keys can't hold Ctrl down)
function Edit-Top($pane) { $r = New-Object FpUi+RECT; [FpUi]::GetWindowRect((Pane-Edits $pane)[0], [ref]$r) | Out-Null; $r.T }
$ses = Join-Path $scratch 'session.ini'
$p = Start-Demo $exe @("--session=$ses", $left, $right)
try {
    Start-Sleep -Milliseconds 600
    $addr = (Pane-Edits $pane0)[0]
    Check 'breadcrumbs: no text box until typing' (-not [FpUi]::IsWindowVisible($addr))
    $s = $script:scale
    Click $pane0 ([int]((Client-Width $pane0) - 30 * $s)) ([int](18 * $s))
    Check 'clicking after the crumbs opens the text box' ([FpUi]::IsWindowVisible($addr))
    Key $addr 0x1B
    Check 'Esc closes it again' (-not [FpUi]::IsWindowVisible($addr))

    $top0 = Edit-Top $pane0
    Post $pane0 $WM_CHAR 0x14; Start-Sleep -Milliseconds 300      # Ctrl+T
    Check 'Ctrl+T opens a tab and shows the strip' ((Edit-Top $pane0) -gt $top0)
    Send-Text $pane0 'sub'
    Key $pane0 0x0D
    Check 'the new tab navigates on its own' (Wait-Until { (Title) -eq 'sub - Folderwing' }) "(title '$(Title)')"
    Click $pane0 ([int](40 * $s)) ([int](15 * $s))
    Post $pane0 0x202 0 ((([int](15 * $s)) -shl 16) -bor [int](40 * $s))
    Check 'clicking the first tab shows it again' (Wait-Until { (Title) -eq 'left - Folderwing' }) "(title '$(Title)')"
    # middle click on the "sub" row (rows: made/, sub/, ...): a tab behind this
    # one; Ctrl+W then closes this one. Strip 30 + pad 6 + address 26 + pad 6 + header 24.
    $rowY = [int]((30 + 6 + 26 + 6 + 24 + 24 + 12) * $s)
    Post $pane0 0x207 0 ((($rowY -band 0xFFFF) -shl 16) -bor [int](80 * $s)); Start-Sleep -Milliseconds 300
    Check 'middle click keeps the shown tab' ((Title) -eq 'left - Folderwing') "(title '$(Title)')"
    Post $pane0 $WM_CHAR 0x17; Start-Sleep -Milliseconds 300      # Ctrl+W
    Check 'Ctrl+W shows the tab opened behind it' (Wait-Until { (Title) -eq 'sub - Folderwing' }) "(title '$(Title)')"
    Post $pane0 $WM_CHAR 0x04; Start-Sleep -Milliseconds 300      # Ctrl+D pins the folder shown

    # Ctrl+D prepended one pin. The home root now follows that collapsed pin;
    # Home alone would revisit sub and leave the long scratch crumbs truncated.
    Key $tree 0x24
    Key $tree 0x28
    Click $tree ([int](80 * $s)) ([int]((6 + 24 + 12) * $s))
    $homeReady=Wait-Until { (Title) -eq "$homeName - Folderwing" }
    Check 'home root follows the newly inserted pin' $homeReady
    if(-not $homeReady){throw 'Breadcrumb input stopped: the home-root fixture was not acknowledged'}
    Click $pane0 ([int](19 * $s)) ([int](36 * $s + 13 * $s))
    Check 'clicking a crumb goes there' (Wait-Until { (Window-Text $addr) -eq 'C:\' }) "(address '$(Window-Text $addr)')"
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}
$saved = if (Test-Path $ses) { Get-Content -Raw $ses } else { '' }
# left pane: C:\ (the tab that was shown) and sub; right pane: one tab
$savedTabs=@(Session-Paths $saved 'tab')
Check 'the session is saved on exit' ($saved -match '\[pane1\]' -and $savedTabs.Count -eq 3) "(paths: $($savedTabs -join ', '))"
Check 'pins are saved' (@(Session-Paths $saved 'pin') -ccontains "$left\sub")

$p = Start-Demo $exe @("--session=$ses")
try {
    Start-Sleep -Milliseconds 600
    Check 'restart restores the shown tab' ((Window-Text (Pane-Edits $pane0)[0]) -eq 'C:\') "(address '$(Window-Text (Pane-Edits $pane0)[0])')"
    Check 'restart restores the tab strip' ((Edit-Top $pane0) -gt (Edit-Top $pane1))
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}

# the function registry: listing, switching off, rebinding, hiding from menus
$ini = Join-Path $scratch 'openpilot.ini'
Set-Content $ini @"
[functions]
svc.icons = off
menu.explorer = off
file.new_file = off
view.breadcrumbs = off
[keys]
file.new_folder = F8
[menu]
file.copy_path = off
[palette]
view.dual = off
[menu.context]
style = native
"@
$listing = & $exe --functions "--settings=$ini" | Out-String
Check '--functions lists the registry' ($listing -match 'file\.rename\s+cold\s+on\s+F2' -and $listing -match 'debug\.panel\s+warm')
Check '--functions shows settings' ($listing -match 'file\.new_folder\s+cold\s+on\s+F8\s' -and $listing -match 'svc\.icons\s+hot\s+off')
Check '--functions shows palette exposure' ($listing -match 'view\.dual[^\n]*\n\s+hidden from the palette')

$csv = Join-Path $scratch 'functions.csv'
$p = Start-Demo $exe @("--settings=$ini", "--profile-out=$csv", $left, $right)
try {
    Start-Sleep -Milliseconds 600
    Check 'breadcrumbs off: a plain text box' ([FpUi]::IsWindowVisible((Pane-Edits $pane0)[0]))
    Key $pane0 0x76    # F7: no longer bound
    Start-Sleep -Milliseconds 300
    Check 'an unbound key does nothing' (-not (Test-Path "$left\New folder"))
    Key $pane0 0x77    # F8: rebound
    Check 'a rebound key runs its function' (Wait-Until { Test-Path "$left\New folder" })
    Start-Sleep -Milliseconds 300
    Key $pane0 0x1B    # leave the rename box
    Key $pane0 0x24    # Home: a folder row
    Post $pane0 $WM_CONTEXTMENU $pane0 -1
    $menu = Open-Menu 5000 '#32768'
    Check 'native style, no Explorer items: Windows'' menu opens' ($null -ne $menu)
    if ($menu) {
        Check 'menu has only app items' ($menu.Items -contains 'Open in other pane' -and -not ($menu.Items -contains $more)) "(items: $($menu.Items -join ', '))"
        Check 'items Explorer''s menu has are left to it' (-not ($menu.Items -contains 'Properties') -and -not ($menu.Items -contains 'Rename'))
        Check 'a function hidden from menus is not shown' (-not ($menu.Items -contains 'Copy path'))
        Post $menu.Window $WM_KEYDOWN 0x1B (Key-LParam 0x1B)
        Start-Sleep -Milliseconds 300
    }
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}
$rows = if (Test-Path $csv) { @(Import-Csv $csv) } else { @() }
$fns = @{}; $rows | Where-Object kind -eq 'fn' | ForEach-Object { $fns[$_.name] = $_ }
Check 'switched-off services never start' ($fns['svc.icons'].value -eq 'off' -and $fns['menu.explorer'].value -eq 'off') "(icons: $($fns['svc.icons'].value))"
Check 'hot services run' ($fns['svc.shell'].value -eq 'running' -and $fns['svc.drop'].value -eq 'running')
Check 'the rebound function ran once' ($fns['file.new_folder'].calls -eq '1')
$stuck = @($fns.Values | Where-Object { $_.value -eq 'running' -and $_.name -notlike 'svc.*' -and $_.name -ne 'settings.watch' })
Check 'cold functions are idle afterwards' ($stuck.Count -eq 0) "(running: $($stuck.name -join ', '))"
$viol = ($rows | Where-Object { $_.kind -eq 'metric' -and $_.name -eq 'fn_violations' }).value
Check 'no lifetime violations' ($viol -eq '0') "(got '$viol')"

# the palette: Go to (recent folders), commands, Everything; nothing left once closed
function Palette { [FpUi]::FindWindowExW($script:main, [IntPtr]::Zero, 'OpenPilotPalette', [IntPtr]::Zero) }
function Palette-Edit { [FpUi]::FindWindowExW((Palette), [IntPtr]::Zero, 'Edit', [IntPtr]::Zero) }
$palCsv = Join-Path $scratch 'palette.csv'
$palSes = Join-Path $scratch 'palette_session.ini'
$everything = [FpUi]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, 'EVERYTHING_TASKBAR_NOTIFICATION', [IntPtr]::Zero) -ne [IntPtr]::Zero
$p = Start-Demo $exe @("--session=$palSes", "--profile-out=$palCsv", $left, $right)
try {
    Start-Sleep -Milliseconds 500
    Check 'no palette until asked' ((Palette) -eq [IntPtr]::Zero)
    Send-Text $pane0 'sub'; Key $pane0 0x0D                       # visit sub: now a recent folder
    if(-not(Wait-Until { (Title) -eq 'sub - Folderwing' })){throw 'Palette fixture did not visit its owned recent folder'}
    Key $pane0 0x08
    Wait-Until { (Title) -eq 'left - Folderwing' } | Out-Null

    Post $pane0 $WM_CHAR 0x10; Start-Sleep -Milliseconds 300       # Ctrl+P
    Check 'Ctrl+P opens Go to' ([FpUi]::IsWindowVisible((Palette)))
    Send-Text (Palette-Edit) 'sub'
    Shot $out '9_goto'
    Key (Palette-Edit) 0x0D
    Check 'Go to opens a recent folder' (Wait-Until { (Title) -eq 'sub - Folderwing' }) "(title '$(Title)')"
    Check 'the palette closes after a jump' ((Palette) -eq [IntPtr]::Zero)

    Key $pane0 0x70                                                # F1: commands
    Send-Text (Palette-Edit) 'dual pane'
    Shot $out '10_commands'
    Key (Palette-Edit) 0x0D
    Check 'a command runs from the palette' (-not [FpUi]::IsWindowVisible($pane1))
    Post $pane0 $WM_CHAR 0x10; Start-Sleep -Milliseconds 300
    Send-Text (Palette-Edit) '>dual'
    Key (Palette-Edit) 0x0D
    Check '">" in Go to switches to commands' ([FpUi]::IsWindowVisible($pane1))
    Post $pane0 $WM_CHAR 0x10; Start-Sleep -Milliseconds 300
    Key (Palette-Edit) 0x1B
    Check 'Esc closes the palette' ((Palette) -eq [IntPtr]::Zero)

    if ($everything) {
        Key $pane0 0x08
        Wait-Until { (Title) -eq 'left - Folderwing' } | Out-Null
        $found = $false
        for ($try = 0; $try -lt 4 -and -not $found; $try++) {   # Everything may need a moment for new files
            Post $pane0 $WM_CHAR 0x06; Start-Sleep -Milliseconds 300   # Ctrl+F
            Send-Text (Palette-Edit) 'inner.txt'
            Start-Sleep -Milliseconds 700
            if ($try -eq 0) { Shot $out '11_search' }
            Key (Palette-Edit) 0x0D
            $found = Wait-Until { (Title) -eq 'sub - Folderwing' } 1000
            if (-not $found -and (Palette) -ne [IntPtr]::Zero) { Key (Palette-Edit) 0x1B }
        }
        Check 'Ctrl+F finds a file with Everything and shows it' $found "(title '$(Title)')"
    } else {
        '  skip  Everything is not running: no search check'
    }
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}
$rows = if (Test-Path $palCsv) { @(Import-Csv $palCsv) } else { @() }
$fns = @{}; $rows | Where-Object kind -eq 'fn' | ForEach-Object { $fns[$_.name] = $_ }
Check 'type icons by default: the Windows icon service never starts' ($fns['svc.icons'].value -eq 'idle' -and $fns['svc.icons'].calls -eq '0') "(svc.icons: $($fns['svc.icons'].value))"
Check 'palette functions ran and are idle' ([int]$fns['palette.jump'].calls -ge 3 -and $fns['palette.jump'].value -eq 'idle' -and
    $fns['palette.commands'].value -eq 'idle') "(jump: $($fns['palette.jump'].calls) $($fns['palette.jump'].value))"
if ($everything) {
    Check 'Everything queries were made and nothing waits' ([int]$fns['search.everything'].calls -ge 1 -and $fns['search.everything'].value -eq 'idle')
}
$viol = ($rows | Where-Object { $_.kind -eq 'metric' -and $_.name -eq 'fn_violations' }).value
Check 'no lifetime violations with the palette' ($viol -eq '0') "(got '$viol')"
$saved = if (Test-Path $palSes) { Get-Content -Raw $palSes } else { '' }
Check 'recent folders are saved' ($saved -match '\[recent\]' -and @(Session-Paths $saved 'folder') -ccontains "$left\sub")

# view.icons style = mixed: programs keep their own icons, so the Windows icon service runs
$mixedIni = Join-Path $scratch 'mixed.ini'
Set-Content $mixedIni "[view.icons]`nstyle = mixed`ncode = tsx"
$mixedCsv = Join-Path $scratch 'mixed.csv'
$listing = & $exe --functions "--settings=$mixedIni" | Out-String
Check '--functions: mixed icons need the Windows icon service' ($listing -notmatch 'not started: the current settings')
$listing = & $exe --functions '--settings=' | Out-String
Check '--functions: type icons leave it unstarted' ($listing -match 'svc\.icons[^\n]*\n(\s+[^\n]*\n)*?\s+not started: the current settings')
$p = Start-Demo $exe @("--settings=$mixedIni", "--profile-out=$mixedCsv", $left)
try {
    Start-Sleep -Milliseconds 800
    Shot $out '12_icons_mixed'
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}
$rows = if (Test-Path $mixedCsv) { @(Import-Csv $mixedCsv) } else { @() }
$icons = $rows | Where-Object { $_.kind -eq 'fn' -and $_.name -eq 'svc.icons' }
Check 'style = mixed starts the Windows icon service' ($icons.value -eq 'running') "(svc.icons: $($icons.value))"

# PDF preview (only when PDFium was fetched and folderwing-pdf.exe built)
if (Test-Path (Join-Path (Split-Path $exe) 'folderwing-pdf.exe')) {
    Add-Type -AssemblyName System.Windows.Forms
    $pl = Join-Path $scratch 'pdf_l'; $pr = Join-Path $scratch 'pdf_r'
    New-Item -ItemType Directory -Force $pl, $pr | Out-Null
    & "$PSScriptRoot\make_pdf.ps1" -Out "$pl\b.pdf" -Pages 6
    & "$PSScriptRoot\make_pdf.ps1" -Out "$pr\r.pdf" -Pages 3
    # Non-preview extensions test linger; another supported preview switches immediately.
    Set-Content "$pl\a.dat" 'a'; Set-Content "$pl\c.dat" 'c'; Set-Content "$pr\q.txt" 'q'
    $pdfIni = Join-Path $scratch 'pdf.ini'
    # original colors: the checks below look for white paper and the light highlight colors
    Set-Content $pdfIni "[preview.pdf]`nlinger = 1`ncolors = original"
    $pdfCsv = Join-Path $scratch 'pdf.csv'
    function Reader { [FpUi]::FindWindowExW($script:main, [IntPtr]::Zero, 'OpenPilotPdf', [IntPtr]::Zero) }
    function Shown($h) { $h -ne [IntPtr]::Zero -and [FpUi]::IsWindowVisible($h) }
    function Helper { Get-CimInstance Win32_Process -Filter "ParentProcessId = $($script:demoPid) AND Name = 'folderwing-pdf.exe'" }
    # share of the reader's area in a color: paper-white (a page was drawn) or the selection's blue
    function Color-Share($h, [scriptblock] $match) {
        $r = New-Object FpUi+RECT
        [FpUi]::GetClientRect($h, [ref]$r) | Out-Null
        $bmp = New-Object Drawing.Bitmap $r.R, $r.B
        $gr = [Drawing.Graphics]::FromImage($bmp); $hdc = $gr.GetHdc()
        [FpUi]::PrintWindow($h, $hdc, 2) | Out-Null
        $gr.ReleaseHdc($hdc); $gr.Dispose()
        $hits = 0; $all = 0
        for ($y = 0; $y -lt $r.B; $y += 3) { for ($x = 0; $x -lt $r.R; $x += 3) {
            $c = $bmp.GetPixel($x, $y); $all++
            if (& $match $c) { $hits++ }
        } }
        $bmp.Dispose()
        if ($all) { $hits / $all } else { 0 }
    }
    function Paper-Share($h) { Color-Share $h { param($c) $c.R -gt 240 -and $c.G -gt 240 -and $c.B -gt 240 } }
    function Selected-Share($h) { Color-Share $h { param($c) $c.B -gt 240 -and $c.R -lt 190 -and $c.R -gt 130 } }
    # mouse messages at page points of make_pdf.ps1's text box (72..540 pt fitted to the width)
    function Pdf-Mouse($h, [int] $msg, [double] $x, [double] $y, [int] $keys = 0) {
        $s = ((Client-Width $h) - 28 * $scale) / 468.0
        $px = [int](14 * $scale + ($x - 72) * $s); $py = [int](30 * $scale + $y * $s)
        Post $h $msg $keys ((($py -band 0xFFFF) -shl 16) -bor ($px -band 0xFFFF))
    }
    function Left-Of($a, $b) {
        $ra = New-Object FpUi+RECT; $rb = New-Object FpUi+RECT
        [FpUi]::GetWindowRect($a, [ref]$ra) | Out-Null; [FpUi]::GetWindowRect($b, [ref]$rb) | Out-Null
        $ra.R -le $rb.L
    }
    $helperPid = $null
    $p = Start-Demo $exe @("--settings=$pdfIni", "--profile-out=$pdfCsv", $pl, $pr)
    try {
        Start-Sleep -Milliseconds 500
        Check 'no reader and no PDF helper before a PDF is shown' ((Reader) -eq [IntPtr]::Zero -and -not (Helper))
        Key $pane0 0x28 0   # a.dat -> b.pdf
        Check 'a PDF under the cursor shows in the right pane' (Wait-Until { Shown (Reader) }) 
        Check 'the right pane gives way to it' (-not [FpUi]::IsWindowVisible($pane1))
        $h = Helper
        $helperPid = $h.ProcessId
        Check 'the PDF helper runs as a child process' ($null -ne $h)
        Check 'a page is drawn' (Wait-Until { (Paper-Share (Reader)) -gt 0.3 }) "(paper $([int]((Paper-Share (Reader)) * 100))%)"
        Shot $out '13_pdf'

        # selection: drag over the first three body lines (baselines 102, 116, 130 pt), then copy
        $clipSaved = Get-Clipboard -Raw
        $rd = Reader
        Pdf-Mouse $rd 0x200 80 99; Start-Sleep -Milliseconds 300   # hover: the page's text is asked for
        Check 'no selection before a drag' ((Selected-Share $rd) -lt 0.001)
        Pdf-Mouse $rd 0x201 72 99 1; Start-Sleep -Milliseconds 50
        Pdf-Mouse $rd 0x200 560 127 1; Start-Sleep -Milliseconds 50
        Pdf-Mouse $rd 0x202 560 127; Start-Sleep -Milliseconds 200
        Check 'dragging highlights the text' (Wait-Until { (Selected-Share $rd) -gt 0.01 }) "($([math]::Round((Selected-Share $rd) * 100, 2))%)"
        Shot $out '13b_pdf_selection'
        # The expected values belong to make_pdf.ps1's private fixture. Each
        # result stands alone: a failed paragraph must not fail a correct word.
        $pdfLines = @(
            'the quick brown fox jumps over a lazy dog while seven bright owls watch from a',
            'n old oak tree near the quiet river bank and count the stars one by one the qu',
            'ick brown fox jumps over a lazy dog while seven bright owls watch from an oldx'
        )
        if (-not ('PdfSmokeClipboard' -as [type])) { Add-Type @'
using System; using System.Runtime.InteropServices;
public static class PdfSmokeClipboard {
 [DllImport("user32.dll")] public static extern uint GetClipboardSequenceNumber();
 [DllImport("user32.dll")] public static extern IntPtr GetClipboardOwner();
 [DllImport("user32.dll")] public static extern IntPtr GetOpenClipboardWindow();
}
'@ }
        function Check-PdfCopy([string]$name,[string[]]$mods,[string]$expected) {
            [FpUi]::OwnedWindow($rd,$script:demoPid)
            $helperBefore=Helper
            $generation="$($helperBefore.ProcessId)/$($helperBefore.CreationDate)"
            Set-Clipboard -Value 'openpilot private PDF copy marker'
            $baseline=[PdfSmokeClipboard]::GetClipboardSequenceNumber()
            # Dispatch and publication share the original total 3000 ms budget.
            $copyClock=[Diagnostics.Stopwatch]::StartNew()
            Chord-Acknowledged $rd $mods 0x43 (Input-Remaining $copyClock 3000)
            $observation=[pscustomobject]@{Text=$null;Sequence=0;After=0;Owner=0;Holder=0;Accepted=$false}
            $remaining=3000-[int]$copyClock.ElapsedMilliseconds
            if($remaining -gt 0){$null=Wait-Until {
                # Wait-Until makes a final predicate call after its own wait;
                # neither that call nor a slow read may accept expired data.
                if($copyClock.ElapsedMilliseconds -ge 3000){return $false}
                $observation.Sequence=[PdfSmokeClipboard]::GetClipboardSequenceNumber()
                $observation.Owner=[PdfSmokeClipboard]::GetClipboardOwner().ToInt64()
                $observation.Holder=[PdfSmokeClipboard]::GetOpenClipboardWindow().ToInt64()
                # Retain this one read. A second read can observe a different
                # publication or contention and is not the result we accepted.
                $observation.Text=Get-Clipboard -Raw -ErrorAction SilentlyContinue
                $observation.After=[PdfSmokeClipboard]::GetClipboardSequenceNumber()
                $observation.Accepted=$copyClock.ElapsedMilliseconds -lt 3000 -and
                    $observation.Sequence -eq $observation.After -and
                    $observation.Sequence -ne $baseline -and $observation.Owner -eq $rd.ToInt64() -and
                    $observation.Text -ceq $expected
                $observation.Accepted
            } $remaining}
            $helperAfter=Helper
            $sameGeneration=$helperBefore -and $helperAfter -and $generation -ceq "$($helperAfter.ProcessId)/$($helperAfter.CreationDate)"
            # No raw clipboard bytes in failure diagnostics; it may have been
            # replaced by another desktop publisher while this test waited.
            $state="(reader=$rd host=$script:demoPid helper=$generation stable=$sameGeneration input=ack copyElapsedMs=$($copyClock.ElapsedMilliseconds) baseline=$baseline sequence=$($observation.Sequence)/$($observation.After) owner=$($observation.Owner) holder=$($observation.Holder) expectedLength=$($expected.Length) actualLength=$($observation.Text.Length))"
            Check $name ($observation.Accepted -and $sameGeneration) $state
        }
        Check-PdfCopy 'Ctrl+C copies the lines as one paragraph' @('Ctrl') ($pdfLines -join ' ')
        Check-PdfCopy 'Ctrl+Shift+C keeps the page''s line breaks' @('Ctrl','Shift') ($pdfLines -join "`r`n")
        Pdf-Mouse $rd 0x203 100 99 1; Start-Sleep -Milliseconds 200
        Check-PdfCopy 'double-click selects a word' @('Ctrl') 'quick'
        Key $rd 0x1B 300
        Check 'Esc clears the selection and keeps the reader' ((Selected-Share $rd) -lt 0.001 -and (Shown (Reader)))
        Set-Clipboard -Value 'before'
        Chord $rd 'Ctrl' 0x43 0
        Check 'Ctrl+C without a selection is the app''s again (copies the PDF file)' (Wait-Until {
            $files = [Windows.Forms.Clipboard]::GetFileDropList()
            $files.Count -eq 1 -and $files[0] -ceq "$pl\b.pdf"
        })
        Chord $rd 'Ctrl' 0x41 100
        Chord $rd 'Ctrl' 0x43 0
        Check 'Ctrl+A, Ctrl+C copies the whole document' (Wait-Until { $t = Get-Clipboard -Raw; $t -and $t.StartsWith('Chapter 1') -and $t.Contains('Chapter 6') })
        Key $rd 0x1B 300
        if ($clipSaved) { Set-Clipboard -Value $clipSaved }

        # find: Ctrl+F from the list opens the reader's find box, not Everything search
        function Hit-Share($h, [switch] $Current) {
            if ($Current) { Color-Share $h { param($c) $c.R -gt 240 -and $c.G -gt 120 -and $c.G -lt 180 -and $c.B -lt 80 } }
            else { Color-Share $h { param($c) $c.R -gt 240 -and $c.G -gt 210 -and $c.B -lt 130 } }
        }
        Chord $pane0 'Ctrl' 0x46 300
        $fe = [FpUi]::FindWindowExW($rd, [IntPtr]::Zero, 'Edit', [IntPtr]::Zero)
        Check 'Ctrl+F with a PDF shown opens find in the PDF' ($fe -ne [IntPtr]::Zero -and [FpUi]::IsWindowVisible($fe))
        Check '...and not Everything search' ((Palette) -eq [IntPtr]::Zero)
        Send-Text $fe 'owls'
        Check 'typing finds and highlights the matches' (Wait-Until { (Hit-Share $rd) -gt 0.002 -and (Hit-Share $rd -Current) -gt 0.0003 })
        Shot $out '13c_pdf_find'
        $top = $rd; $before = Hit-Share $rd -Current
        for ($i = 0; $i -lt 30; $i++) { Key $fe 0x0D 0 }   # Enter x30: far down the document
        Start-Sleep -Milliseconds 500
        Check 'Enter steps through matches and scrolls to them' ((Hit-Share $rd -Current) -gt 0.0003 -and
            (Paper-Share $rd) -gt 0.3)
        Key $fe 0x1B 300
        Check 'Esc closes find and its highlights' (-not [FpUi]::IsWindowVisible($fe) -and (Hit-Share $rd) -lt 0.0005 -and (Shown (Reader)))

        Key $pane0 0x28 300   # -> c.dat
        Check 'it stays a while after the cursor leaves the PDF' (Shown (Reader))
        Check 'then the right pane comes back (linger = 1)' (Wait-Until { -not (Shown (Reader)) -and [FpUi]::IsWindowVisible($pane1) })

        Post $pane1 0x0007 0; Start-Sleep -Milliseconds 200   # WM_SETFOCUS: the right pane is active
        Key $pane1 0x23 0   # End -> r.pdf
        Check 'a PDF in the right pane swaps the panes' (Wait-Until { (Shown (Reader)) -and -not [FpUi]::IsWindowVisible($pane0) })
        Check 'the right pane moves left of the reader' (Left-Of $pane1 (Reader))
        Shot $out '14_pdf_swapped'
        Post (Reader) $WM_KEYDOWN 0x1B; Start-Sleep -Milliseconds 300   # Esc
        Check 'Esc closes it and the panes swap back' (-not (Shown (Reader)) -and [FpUi]::IsWindowVisible($pane0) -and
            (Left-Of $pane0 $pane1))
        Start-Sleep -Milliseconds 300
        Check 'and it stays closed on the same PDF' (-not (Shown (Reader)))

        Post $pane1 $WM_CHAR 0x1C; Start-Sleep -Milliseconds 300   # Ctrl+\: one pane
        Key $pane0 0x26 0   # c.dat -> b.pdf
        Check 'with one pane, the reader takes the right side for a while' (Wait-Until { Shown (Reader) }) 
        Key $pane0 0x26 0   # -> a.dat
        Check 'and leaves one pane again' (Wait-Until { -not (Shown (Reader)) -and -not [FpUi]::IsWindowVisible($pane1) })
    } finally {
        if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
    }
    Start-Sleep -Milliseconds 300
    Check 'the PDF helper exits with the app' (-not $helperPid -or -not (Get-Process -Id $helperPid -ErrorAction SilentlyContinue))
    $rows = if (Test-Path $pdfCsv) { @(Import-Csv $pdfCsv) } else { @() }
    $viol = ($rows | Where-Object { $_.kind -eq 'metric' -and $_.name -eq 'fn_violations' }).value
    Check 'no lifetime violations with the PDF preview' ($viol -eq '0') "(got '$viol')"

    # positions: a PDF read part-way opens there after a restart; one only glanced at isn't remembered
    $pp = Join-Path $scratch 'pdf_pos'
    New-Item -ItemType Directory -Force $pp | Out-Null
    & "$PSScriptRoot\make_pdf.ps1" -Out "$pp\b.pdf" -Pages 6
    & "$PSScriptRoot\make_pdf.ps1" -Out "$pp\c.pdf" -Pages 2
    Set-Content "$pp\a.txt" 'a'
    $posSes = Join-Path $pp 'session.ini'
    $posFile = Join-Path $pp 'pdf-positions.txt'
    $p = Start-Demo $exe @("--session=$posSes", $pp)
    try {
        Start-Sleep -Milliseconds 400
        Key $pane0 0x28 0   # b.pdf
        $null = Wait-Until { Shown (Reader) }
        Start-Sleep -Milliseconds 500
        for ($i = 0; $i -lt 5; $i++) { Key (Reader) 0x22 80 }   # PgDn x5: page 2 or so
        Start-Sleep -Milliseconds 300
        Key $pane0 0x28 0   # c.pdf: only a glance
        Start-Sleep -Milliseconds 800
    } finally {
        if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
    }
    $bLine = "\t-?\d+\t[1-5]\t[\d.]+\t0\.000\t0\.0\t$([regex]::Escape("$pp\b.pdf"))"
    $saved = if (Test-Path $posFile) { Get-Content -Raw $posFile } else { '' }
    Check 'where a PDF was left is saved on exit' ($saved -match $bLine)
    Check 'a PDF only glanced at is not' ($saved -notmatch 'c\.pdf')
    $p = Start-Demo $exe @("--session=$posSes", $pp)
    try {
        Start-Sleep -Milliseconds 400
        Key $pane0 0x28 0   # b.pdf again, closed without scrolling
        $null = Wait-Until { Shown (Reader) }
        Start-Sleep -Milliseconds 800
        Shot $out '13e_pdf_restored'
    } finally {
        if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
    }
    # a restore that failed would read back as page 0 and overwrite the line
    $again = if (Test-Path $posFile) { Get-Content -Raw $posFile } else { '' }
    $was = [regex]::Match($saved, "\d+$bLine").Value
    $now = [regex]::Match($again, "\d+$bLine").Value
    Check 'after a restart it opens where it was left' ($was -and $now -eq $was) "(was '$was', now '$now')"

    # colors: mixed by default (a text PDF comes out dark), switched from the command palette
    function Dark-Share($h) { Color-Share $h { param($c) $c.R -ge 34 -and $c.R -le 42 -and $c.G -ge 34 -and $c.G -le 42 -and $c.B -ge 34 -and $c.B -le 42 } }
    $p = Start-Demo $exe @($pl)
    try {
        Start-Sleep -Milliseconds 400
        Key $pane0 0x28 0
        $null = Wait-Until { Shown (Reader) }
        Check 'by default a text PDF is shown dark' (Wait-Until { (Dark-Share (Reader)) -gt 0.3 -and (Paper-Share (Reader)) -lt 0.02 }) "(dark $([int]((Dark-Share (Reader)) * 100))%, white $([int]((Paper-Share (Reader)) * 100))%)"
        Shot $out '13d_pdf_dark'
        foreach ($i in 1, 2) { # mixed -> dark -> original
            Key $pane0 0x70 300
            Send-Text (Palette-Edit) 'PDF colors'
            Key (Palette-Edit) 0x0D 300
        }
        Check 'the palette switches it to original colors' (Wait-Until { (Paper-Share (Reader)) -gt 0.3 })
    } finally {
        if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
    }
} else {
    '  skip  PDF preview (no folderwing-pdf.exe: run ./tools/get-pdfium.ps1)'
}

# WSL: a folder in the default distro is listed and watched by the Linux helper
$wslOwner=[Guid]::NewGuid().ToString('N');$wslRoot='';$distro='';$wslSetup=$null
. "$PSScriptRoot\ui_smoke_wsl.ps1"
if(Get-Command wsl.exe -ErrorAction SilentlyContinue){
    $wslSetup=Invoke-SmokeWsl setup
    $lines=@(($wslSetup.output -split '\r?\n')|Where-Object {$_})
    if($lines.Count -eq 2 -and $lines[0] -match '^[\w.-]+$' -and $lines[1] -match '^/tmp/openpilot-ui-[a-zA-Z0-9]{8}$'){$distro=$lines[0];$wslRoot=$lines[1]}
}
if ($wslSetup -and $wslSetup.finished -and $wslSetup.exit_code -eq 0 -and $wslRoot) {
    $wslName=Split-Path $wslRoot -Leaf
    $wd = "\\wsl.localhost\$distro"+$wslRoot.Replace('/','\')
    $wslReport = Join-Path $scratch 'wsl_profile.txt'
    function Wsl-Helpers { @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $($script:demoPid) AND Name = 'wsl.exe'") }
    $p = Start-Demo $exe @("--profile-out=$wslReport", $wd, $left)
    try {
        Check 'a WSL folder opens' (Wait-Until { (Title) -eq "$wslName - Folderwing" }) "(title '$(Title)')"
        Check 'its Linux helper runs under OpenPilot' (Wait-Until { (Wsl-Helpers).Count -eq 1 } 5000)
        Start-Sleep -Milliseconds 800
        $mutation=Invoke-SmokeWsl mutation
        Check 'the owned Linux fixture mutation completes' ($mutation.finished -and $mutation.exit_code -eq 0)
        if(-not $mutation.finished -or $mutation.exit_code -ne 0){throw 'WSL service/fixture mutation failed; dependent watch and link inputs stopped; evidence retained'}
        Start-Sleep -Milliseconds 700
        Send-Text $pane0 'born'
        Key $pane0 0x0D
        Check 'a folder made in Linux shows up by itself' (Wait-Until { (Title) -eq 'born - Folderwing' }) "(title '$(Title)')"
        Key $pane0 0x08
        $null = Wait-Until { (Title) -eq "$wslName - Folderwing" }
        Send-Text $pane0 'lnk'
        Key $pane0 0x0D
        Check 'a symlinked folder opens' (Wait-Until { (Title) -eq 'lnk - Folderwing' }) "(title '$(Title)')"
        Key $pane0 0x08
        $null = Wait-Until { (Title) -eq "$wslName - Folderwing" }
        Start-Sleep -Milliseconds 300
        Shot $out '14_wsl'
    } finally {
        if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
    }
    Check 'closing OpenPilot ends the helper' (Wait-Until { (Wsl-Helpers).Count -eq 0 })
    $text = if (Test-Path $wslReport) { Get-Content -Raw $wslReport } else { '' }
    Check 'the folder was listed by Linux' ($text -match 'wsl\.start' -and $text -match 'wsl\.list\s')
} else {
    if(Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss'){
        Check 'installed WSL can prepare its owned smoke fixture' $false '(see wsl-setup.json; no dependent WSL inputs sent)'
    }else{'  skip  WSL folders (no WSL distro)'}
}

# the settings in the side panel: their app runs while they show
function Panel-Apps { @(Get-Process folderwing-settings -ErrorAction SilentlyContinue | Where-Object { $_.Parent.Id -eq $script:demoPid }) }
$p = Start-Demo $exe @($left)
try {
    Start-Sleep -Milliseconds 400
    Chord $pane0 'Ctrl' 0xBC # Ctrl+Comma
    $side = [FpUi]::FindWindowExW($main, [IntPtr]::Zero, 'OpenPilotSide', [IntPtr]::Zero)
    $slot = [FpUi]::FindWindowExW($side, [IntPtr]::Zero, 'OpenPilotSideSlot', [IntPtr]::Zero)
    Check 'Ctrl+, opens the side panel' ([FpUi]::IsWindowVisible($side))
    Check 'its app starts' (Wait-Until { (Panel-Apps).Count -eq 1 })
    Check 'the panes make room for it' ((Client-Width $pane1) -lt ((Client-Width $main) / 3)) "(right pane $(Client-Width $pane1) px)"
    Start-Sleep -Milliseconds 300
    Shot $out '15_settings'
    Chord $slot 'Ctrl' 0xBC
    Check 'Ctrl+, closes it again' (-not [FpUi]::IsWindowVisible($side))
    Check 'its app ends' (Wait-Until { (Panel-Apps).Count -eq 0 })
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}

# settings while running: a saved file applies at once; a frozen or killed
# settings app never holds up the explorer
if (-not ('FpSmoke' -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices; using System.Diagnostics;
public static class FpSmoke {
  [DllImport("user32.dll")] static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint ms, out IntPtr r);
  [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
  [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
  public static double Ping(IntPtr h) {
    var sw = Stopwatch.StartNew(); IntPtr r;
    return SendMessageTimeoutW(h, 0, IntPtr.Zero, IntPtr.Zero, 0, 5000, out r) == IntPtr.Zero ? -1 : sw.Elapsed.TotalMilliseconds;
  }
}
"@
}
$liveIni = Join-Path $scratch 'live.ini'
$liveReport = Join-Path $scratch 'live-profile.txt'
"; empty for now`n" | Set-Content $liveIni
$p = Start-Demo $exe @("--settings=$liveIni", "--profile-out=$liveReport", $left)
try {
    Start-Sleep -Milliseconds 600
    $addr = (Pane-Edits $pane0)[0]
    Check 'breadcrumbs at first (the address box is hidden)' (-not [FpUi]::IsWindowVisible($addr))
    "[functions]`nview.breadcrumbs = off`n" | Set-Content $liveIni
    Check 'a saved settings file applies at once' (Wait-Until { [FpUi]::IsWindowVisible($addr) } 3000)
    "; back to the defaults`n" | Set-Content $liveIni
    Check 'and so does undoing it' (Wait-Until { -not [FpUi]::IsWindowVisible($addr) } 3000)

    $r = New-Object FpUi+RECT; [FpUi]::GetClientRect($main, [ref]$r) | Out-Null
    Click $main ($r.R - 10) ($r.B - 8) # the gear
    $side = [FpUi]::FindWindowExW($main, [IntPtr]::Zero, 'OpenPilotSide', [IntPtr]::Zero)
    $slot = [FpUi]::FindWindowExW($side, [IntPtr]::Zero, 'OpenPilotSideSlot', [IntPtr]::Zero)
    Check 'the gear opens the settings' ((Wait-Until { (Panel-Apps).Count -eq 1 }) -and [FpUi]::IsWindowVisible($side))
    Start-Sleep -Milliseconds 400
    $app = (Panel-Apps)[0]
    [FpSmoke]::NtSuspendProcess($app.Handle) | Out-Null
    try {
        Start-Sleep -Milliseconds 1500
        $worst = 0.0
        foreach ($i in 1..10) {
            $t = [FpSmoke]::Ping($main); if ($t -lt 0) { $t = 5000 }; if ($t -gt $worst) { $worst = $t }
            Click $slot 50 20; Post $slot $WM_CHAR 0x61; Start-Sleep -Milliseconds 30
        }
        "[preview.pdf]`nlinger = 3`n" | Set-Content $liveIni
        Start-Sleep -Milliseconds 800
        $t = [FpSmoke]::Ping($main); if ($t -lt 0 -or $t -gt $worst) { $worst = if ($t -lt 0) { 5000 } else { $t } }
        Check 'a frozen settings app holds nothing up' ($worst -lt 200) "(worst answer $([math]::Round($worst)) ms)"
    } finally {
        [FpSmoke]::NtResumeProcess($app.Handle) | Out-Null
    }
    Start-Sleep -Milliseconds 600
    Shot $out '16_settings_live'
    Stop-Process -Id $app.Id -Force
    Check 'a killed settings app leaves the explorer running' ((Wait-Until { (Panel-Apps).Count -eq 0 }) -and [FpSmoke]::Ping($main) -ge 0)
    Chord $pane0 'Ctrl' 0xBC
    Wait-Until { -not [FpUi]::IsWindowVisible($side) } 2000 | Out-Null
    Chord $pane0 'Ctrl' 0xBC
    Check 'and opening the settings again starts a new one' (Wait-Until { (Panel-Apps).Count -eq 1 }) "(panel shown: $([FpUi]::IsWindowVisible($side)))"

    # Native input reaches the host before the helper. Keep each save and
    # its file/UI assertion within the existing 2000ms result deadline.
    Start-Sleep -Milliseconds 500
    $settingsTarget=New-SettingsTarget
    function Live-Click([int]$x,[int]$y,[int]$status,[scriptblock]$result) {
        $clock=[Diagnostics.Stopwatch]::StartNew();$before=Settings-State $settingsTarget (Input-Remaining $clock 2000)
        $null=Send-SettingsClick $settingsTarget $x $y (Input-Remaining $clock 2000)
        $null=Wait-SettingsSave $settingsTarget $before $status $clock 2000
        Wait-Until $result (Input-Remaining $clock 2000)
    }
    function Live-Search([string]$query) {
        $clock=[Diagnostics.Stopwatch]::StartNew()
        $null=Send-SettingsKey $settingsTarget 0x1B @() (Input-Remaining $clock 2000)
        $null=Send-SettingsKey $settingsTarget 0x1B @() (Input-Remaining $clock 2000)
        $null=Assert-SettingsFocus $settingsTarget '' 0 '' (Input-Remaining $clock 2000)
        $null=Send-SettingsText $settingsTarget $query (Input-Remaining $clock 2000)
        $null=Assert-SettingsFocus $settingsTarget $query 0 '' (Input-Remaining $clock 2000)
    }
    "; mine, kept`n[keys]`nview.refresh = F5, Ctrl+R`n" | Set-Content $liveIni
    Start-Sleep -Milliseconds 800
    $sw = Client-Width $slot
    $null=Send-SettingsText $settingsTarget 'breadcrumbs' 2000
    $null=Assert-SettingsFocus $settingsTarget 'breadcrumbs' 0 '' 2000
    Check 'the On/Off switch in the panel saves and applies at once' (Live-Click ($sw - 30) 136 1 {
        [FpUi]::IsWindowVisible($addr) -and (Get-Content -Raw $liveIni) -match '(?s)^; mine, kept.*view\.refresh = F5, Ctrl\+R.*\[functions\]\s+view\.breadcrumbs = off'
    })
    Shot $out '17_settings_edit'
    Add-Content $liveIni "[preview.pdf]`nlinger = 4"
    Check 'a change made on an older file is refused, and the newer file applies' (Live-Click ($sw - 30) 136 2 {
        $ini=Get-Content -Raw $liveIni
        $ini -match 'linger = 4' -and $ini -match 'view\.breadcrumbs = off' -and [FpUi]::IsWindowVisible($addr)
    })
    Shot $out '18_settings_conflict'
    Check 'then it saves; the default removes the line' (Live-Click ($sw - 30) 136 1 {
        -not [FpUi]::IsWindowVisible($addr) -and (Get-Content -Raw $liveIni) -notmatch 'breadcrumbs' -and (Get-Content -Raw $liveIni) -match 'linger = 4'
    })
    Live-Search 'preview.pdf colors'
    $null=Send-SettingsClick $settingsTarget ($sw - 60) 211 2000
    $null=Wait-SettingsState $settingsTarget {param($s) $s.FieldOpen -eq 1 -and $s.FieldSaving -eq 0} ([Diagnostics.Stopwatch]::StartNew()) 2000 'linger editor'
    $null=Send-SettingsKey $settingsTarget 0x41 @('Ctrl') 2000
    $null=Send-SettingsText $settingsTarget '99999' 2000
    $null=Invoke-SettingsSave $settingsTarget 0x0D @() 3 300
    Check 'a value that does not fit is refused' ((Get-Content -Raw $liveIni) -match 'linger = 4')
    Shot $out '19_settings_refused'
    $null=Send-SettingsKey $settingsTarget 0x41 @('Ctrl') 2000
    $null=Send-SettingsText $settingsTarget '7' 2000
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $null=Invoke-SettingsSave $settingsTarget 0x0D @() 1 (Input-Remaining $clock 2000)
    $null=Wait-SettingsState $settingsTarget {param($s) $s.FieldOpen -eq 0} $clock 2000 'saved editor closed'
    Check 'a typed value saves on Enter' ((Wait-Until { (Get-Content -Raw $liveIni) -match 'linger = 7' } (Input-Remaining $clock 2000)))
    Live-Search 'file.new_folder'

    # Exercise the real Tab order. Acknowledge each intended control before
    # acting, so a missed transition cannot cascade into four unrelated checks.
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $keySub=0
    foreach ($kind in @(1,2,2,3,5,6)) {
        $s=Send-SettingsKey $settingsTarget 0x09 @() (Input-Remaining $clock 2000)
        $null=Assert-SettingsFocus $settingsTarget 'file.new_folder' $kind 'file.new_folder' (Input-Remaining $clock 2000)
        if($kind -eq 2 -and $s.FocusSub -ne $keySub){throw "Unexpected key-removal Tab target: $s"}
        if($kind -eq 2){$keySub++}
    }
    $null=Invoke-SettingsSave $settingsTarget 0x20 @() 1 (Input-Remaining $clock 2000)
    Check 'Tab reaches a switch and Space uses it (the palette)' (Wait-Until { (Get-Content -Raw $liveIni) -match '\[palette\]\s+file\.new_folder = off' } (Input-Remaining $clock 2000))
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $null=Send-SettingsKey $settingsTarget 0x09 @('Shift') (Input-Remaining $clock 2000)
    $null=Assert-SettingsFocus $settingsTarget 'file.new_folder' 5 'file.new_folder' (Input-Remaining $clock 2000)
    $null=Invoke-SettingsSave $settingsTarget 0x0D @() 1 (Input-Remaining $clock 2000)
    Check 'Shift+Tab goes back, Enter uses it (context menus)' (Wait-Until { (Get-Content -Raw $liveIni) -match '\[menu\]\s+file\.new_folder = off' } (Input-Remaining $clock 2000))
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $null=Send-SettingsKey $settingsTarget 0x09 @('Shift') (Input-Remaining $clock 2000)
    $null=Assert-SettingsFocus $settingsTarget 'file.new_folder' 3 'file.new_folder' (Input-Remaining $clock 2000)
    $null=Send-SettingsKey $settingsTarget 0x0D @() (Input-Remaining $clock 2000)
    $null=Wait-SettingsState $settingsTarget {param($s) $s.Recording -eq 1} $clock 2000 'key recording'
    $null=Invoke-SettingsSave $settingsTarget 0x4E @('Ctrl','Alt') 1 (Input-Remaining $clock 2000)
    Check 'a recorded key is added, named as the file names keys' (Wait-Until { (Get-Content -Raw $liveIni) -match 'file\.new_folder = F7, Ctrl\+Shift\+N, Ctrl\+Alt\+N' } (Input-Remaining $clock 2000))
    $null=Send-SettingsKey $settingsTarget 0x0D @() 2000
    $null=Wait-SettingsState $settingsTarget {param($s) $s.Recording -eq 1} ([Diagnostics.Stopwatch]::StartNew()) 2000 'second key recording'
    $null=Send-SettingsKey $settingsTarget 0x52 @('Ctrl') 2000
    $null=Wait-SettingsState $settingsTarget {param($s) $s.Conflict -eq 1 -and $s.FocusKind -eq 8} ([Diagnostics.Stopwatch]::StartNew()) 2000 'Move it here focus'
    Shot $out '20_settings_key_conflict'
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $null=Invoke-SettingsSave $settingsTarget 0x0D @() 1 (Input-Remaining $clock 2000)
    Check 'a key in use is offered to move, and moving it saves both' (Wait-Until {
        $t = Get-Content -Raw $liveIni; $t -match 'view\.refresh = F5\r?\n' -and $t -match 'file\.new_folder = F7, Ctrl\+Shift\+N, Ctrl\+Alt\+N, Ctrl\+R' } (Input-Remaining $clock 2000))
    Live-Search ''
    Send-AcknowledgedKey $slot 0x1B 2000 # closing cannot await a helper snapshot
    Check 'Esc in the settings closes them' ((Wait-Until { -not [FpUi]::IsWindowVisible($side) }) -and (Wait-Until { (Panel-Apps).Count -eq 0 }))
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}
$text = if (Test-Path $liveReport) { Get-Content -Raw $liveReport } else { '' }
Check 'the settings were applied 12 times (7 saved by the panel), the app saw it hang' (
    $text -match 'settings\.reload\s+12\s' -and $text -match 'settings\.save\s+7\s' -and $text -match 'panel\.hung\s+1') "(report: $($text.Length) chars)"

# relays: menu items of your own from [relay.NAME] sections
$rel = Join-Path $scratch 'relays'
New-Item -ItemType Directory -Force "$rel\sub" | Out-Null
Set-Content "$rel\a.txt" 'a'
$relIni = Join-Path $scratch 'relays.ini'
$relCsv = Join-Path $scratch 'relays.csv'
Set-Content $relIni @"
[relay.list]
title = Write list
run = cmd /c echo %* > "%D\list.txt"
window = hidden
[relay.note]
title = New note
new = New note.md
[relay.zip]
title = Zip it
sendto = Compressed (zipped) folder
types = txt
on = files
[relay.broken]
run = x
item = y
"@
$listing = & $exe --functions "--settings=$relIni" | Out-String
Check 'relays are functions, and a broken one is reported' ($listing -match 'relay\.list\s+cold\s+on' -and $listing -match 'relay\.broken\] has more than one of')
$p = Start-Demo $exe @("--settings=$relIni", "--profile-out=$relCsv", $rel, $right)
try {
    Start-Sleep -Milliseconds 600
    Key $pane0 0x28 # Down: a.txt (folders come first)
    Post $pane0 $WM_CONTEXTMENU $pane0 -1
    $menu = Open-Menu 3000 'OpenPilotMenu'
    Check 'a file''s menu has its relays, above Properties' ($menu -and $menu.Items -contains 'Write list' -and $menu.Items -contains 'Zip it' -and
        [array]::IndexOf($menu.Items, 'Zip it') -lt [array]::IndexOf($menu.Items, 'Properties') -and -not ($menu.Items -contains 'New note') -and
        -not ($menu.Items -contains 'broken')) "(items: $($menu.Items -join ', '))"
    if ($menu) { Check 'a command line relay runs' ((Pick-Menu $menu 'Write list') -and (Wait-Until { (Test-Path "$rel\list.txt") -and (Get-Content -Raw "$rel\list.txt") -match [regex]::Escape("`"$rel\a.txt`"") } 3000)) }
    Key $pane0 0x24 # Home: sub, a folder
    Post $pane0 $WM_CONTEXTMENU $pane0 -1
    $menu = Open-Menu 3000 'OpenPilotMenu'
    Check 'types and on keep a relay to files' ($menu -and -not ($menu.Items -contains 'Zip it') -and $menu.Items -contains 'Write list')
    if ($menu) { Close-SmokeMenu $menu }
    $r = New-Object FpUi+RECT; [FpUi]::GetWindowRect($pane0, [ref]$r) | Out-Null
    Post $pane0 $WM_CONTEXTMENU $pane0 ((([int]$r.B - 40) -shl 16) -bor ([int]$r.L + 120))
    $menu = Open-Menu 3000 'OpenPilotMenu'
    Check 'the empty space has the new file relay' ($menu -and $menu.Items -contains 'New note' -and -not ($menu.Items -contains 'Write list')) "(items: $($menu.Items -join ', '))"
    if ($menu) {
        # Existing keyboard/mouse highlighting must not offset the helper's pick.
        Post $menu.Window $WM_KEYDOWN 0x24 (Key-LParam 0x24)
        Pick-Menu $menu 'New note' | Out-Null
        $made = Wait-Until { Test-Path "$rel\New note.md" }
        $renaming = $made -and (Wait-Until { (Pane-Edits $pane0).Count -eq 2 })
        Check 'a new file relay makes the file and renames it' ($made -and $renaming) "(created: $made; rename editor: $renaming; edits: $((Pane-Edits $pane0).Count))"
        $edits = Pane-Edits $pane0; if ($edits.Count -eq 2) { Key $edits[1] 0x1B }
    }
    Key $pane0 0x24; Key $pane0 0x28 # a.txt again
    Post $pane0 $WM_CONTEXTMENU $pane0 -1
    $menu = Open-Menu 3000 'OpenPilotMenu'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if ($menu) { Check 'a Send to relay runs in folderwing-shell.exe' ((Pick-Menu $menu 'Zip it') -and (Wait-Until { Test-Path "$rel\a.zip" } 8000)) "($($sw.ElapsedMilliseconds) ms)" }
    Check 'folderwing-shell.exe runs apart from OpenPilot' (@(Get-Process folderwing-shell -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$(Split-Path $exe)*" }).Count -ge 1)
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
}
$rows = if (Test-Path $relCsv) { @(Import-Csv $relCsv) } else { @() }
$fns = @{}; $rows | Where-Object kind -eq 'fn' | ForEach-Object { $fns[$_.name] = $_ }
Check 'relays ran once each and are idle' ($fns['relay.list'].calls -eq '1' -and $fns['relay.zip'].calls -eq '1' -and $fns['relay.note'].value -eq 'idle')
Check 'folderwing-shell.exe leaves after OpenPilot closes' (Wait-Until { @(Get-Process folderwing-shell -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$(Split-Path $exe)*" }).Count -eq 0 } 6000)

# the terminal in the side panel (folderwing-terminal.exe, built when libvterm is there)
if (Test-Path (Join-Path (Split-Path $exe) 'folderwing-terminal.exe')) {
    function Term-Apps { @(Get-Process folderwing-terminal -ErrorAction SilentlyContinue | Where-Object { $_.Parent.Id -eq $script:demoPid }) }
    $term = Join-Path $scratch 'term'
    New-Item -ItemType Directory -Force "$term\sub" | Out-Null
    $termIni = Join-Path $scratch 'term.ini'
    Set-Content $termIni @"
[shell.terminal]
shell = cmd
[relay.where]
title = Where am I
on = background
run = cmd /c cd > "%D\relay.txt"
"@
    $p = Start-Demo $exe @("--settings=$termIni", $term, $right)
    try {
        Start-Sleep -Milliseconds 500
        Chord $pane0 'Ctrl' 0xC0 # Ctrl+`
        $side = [FpUi]::FindWindowExW($main, [IntPtr]::Zero, 'OpenPilotSide', [IntPtr]::Zero)
        $slot = [FpUi]::FindWindowExW($side, [IntPtr]::Zero, 'OpenPilotSideSlot', [IntPtr]::Zero)
        Check 'Ctrl+` opens the terminal in the side panel' ((Wait-Until { (Term-Apps).Count -eq 1 }) -and [FpUi]::IsWindowVisible($side))
        $termPid = if ((Term-Apps).Count) { (Term-Apps)[0].Id } else { 0 }
        Start-Sleep -Milliseconds 1500 # cmd starts and shows its prompt
        Send-Text $slot 'cd sub'
        Key $slot 0x0D
        Check 'the pane follows the shell''s cd' (Wait-Until { (Title) -eq 'sub - Folderwing' }) "(title '$(Title)')"
        Key $pane0 0x08 # Backspace: the pane goes up, the idle shell follows it
        Wait-Until { (Title) -eq 'term - Folderwing' } | Out-Null
        Start-Sleep -Milliseconds 800
        Send-Text $slot 'cd > here.txt'
        Key $slot 0x0D
        Check 'an idle shell follows the pane' (Wait-Until { (Test-Path "$term\here.txt") -and (Get-Content "$term\here.txt") -eq $term }) "(here.txt: $(Get-ChildItem -Recurse $term -Filter here.txt | ForEach-Object FullName))"
        Shot $out '23_terminal'
        Chord $slot 'Ctrl' 0xBC # Ctrl+,
        Check 'the settings show beside the running terminal' ((Wait-Until { (Panel-Apps).Count -eq 1 }) -and (Term-Apps).Count -eq 1)
        Chord $slot 'Ctrl' 0xBC
        Check 'closing the panel ends the settings, the terminal keeps running' ((Wait-Until { (Panel-Apps).Count -eq 0 }) -and
            -not [FpUi]::IsWindowVisible($side) -and (Term-Apps).Count -eq 1 -and (Term-Apps)[0].Id -eq $termPid)
        $r = New-Object FpUi+RECT; [FpUi]::GetWindowRect($pane0, [ref]$r) | Out-Null
        Post $pane0 $WM_CONTEXTMENU $pane0 ((([int]$r.B - 40) -shl 16) -bor ([int]$r.L + 120))
        $menu = Open-Menu 3000 'OpenPilotMenu'
        if ($menu) { Pick-Menu $menu 'Where am I' | Out-Null }
        Check 'a console relay runs in the panel terminal' ((Wait-Until { Test-Path "$term\relay.txt" } 4000) -and [FpUi]::IsWindowVisible($side) -and
            (Term-Apps).Count -eq 1 -and (Term-Apps)[0].Id -eq $termPid)
        Start-Sleep -Milliseconds 500
        Shot $out '24_terminal_relay'
    } finally {
        if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
    }
    Check 'the terminal and its shells end with OpenPilot' (Wait-Until { @(Get-Process folderwing-terminal -ErrorAction SilentlyContinue | Where-Object { $_.Id -eq $termPid }).Count -eq 0 } 3000)
} else {
    '  skip  the panel terminal (no folderwing-terminal.exe: run ./tools/get-libvterm.ps1, then build)'
}

# --profile-out profiles from launch and writes the report on close
$report = Join-Path $scratch 'profile.txt'
$p = Start-Demo $exe @("--profile-out=$report", $left)
try {
    Start-Sleep -Milliseconds 500
    Check '--profile-out stays headless' (-not [FpUi]::IsWindowVisible($dbg))
} finally {
    if (-not (Stop-Demo $p)) { '  FAIL  window did not close'; $script:failed++ }
    $text = if (Test-Path $report) { Get-Content -Raw $report } else { '' }
    Check '--profile-out writes a report on close' ($text -match 'dir\.read' -and $text -match 'pane\.' -and $text -match 'ui\.dispatch') "(got $($text.Length) chars)"
    Check 'shell starts after the first frame' ($text -match 'shell\.init' -and $text -match 'shell ready')
    Check 'the report lists running functions' ($text -match 'functions\s+\d+, \d+ on, \d+ running' -and $text -notmatch 'VIOLATIONS')
}
"screenshots: $out"
if(-not $script:failed -and $wslRoot){$cleanup=Invoke-SmokeWsl cleanup;Check 'owned Linux fixture cleanup completes' ($cleanup.finished -and $cleanup.exit_code -eq 0)}
@{passed=$script:passed;failed=$script:failed;separate_explorer_menu=[bool]$SeparateExplorerMenu;scratch=$scratch;retries=0}|ConvertTo-Json|Set-Content (Join-Path $out 'smoke-result.json')
if(-not $script:failed){Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue}
exit $script:failed
