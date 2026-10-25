<# Ordinary owned Settings fixture. Validate native capture and activation before
   admitting any OS keyboard input. No forced focus or thread-input attachment. #>
param([string]$Config='release',[string]$EvidenceRoot='',[switch]$CompileOnly,[switch]$ContractOnly,[ValidateSet('prerequisite','N08','N10-same','N10-count','N09','N12-same','N12-switch','N12-reopen','N12-reload')][string]$Case='prerequisite')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
Add-Type -Path "$PSScriptRoot\settings_ui_native.cs"
if($ContractOnly){
 $contracts=@([OwnedSettingsInput]::InputContracts());$clipboardContracts=@([OwnedSettingsInput]::ClipboardBoundaryContracts())
 foreach($pair in @(@($null,1),@([ushort[]]@(),1),@([ushort[]]@(16),2),@([ushort[]]@(17),2),@([ushort[]]@(17,16),3))){
  $t=[OwnedSettingsInput]::DescribeKey(65,[ushort[]]$pair[0]);if($t.KeyDownPackets -ne $pair[1] -or -not $t.Acknowledged(10,10+$pair[1])){throw 'C# transaction interop count/ack contract failed'}
 }
 if(@(($contracts+$clipboardContracts)|Where-Object {-not $_.Passed}).Count){throw 'Input transaction/clipboard boundary pure contract failed'}
 @{outcome='passed';contracts=$contracts;clipboard_contracts=$clipboardContracts;interop_cases=5;os_input=0;source='single C# packet transaction, pure fake sender for partial prefixes'}|ConvertTo-Json -Depth 7
 return
}
if($CompileOnly){'Native GDI/ordinary-input fixture compiled';return}
$root=Split-Path $PSScriptRoot -Parent
function Os-Key([int]$vk,[ushort[]]$mods=@()){
 $c=[Diagnostics.Stopwatch]::StartNew();$b=Settings-State $script:target 2000
 $transaction=[OwnedSettingsInput]::Key($script:main,$script:target.Slot,$script:demoPid,[ushort]$vk,$mods)
 $s=Wait-SettingsState $script:target {param($s)$transaction.Acknowledged($b.Keys,$s.Keys)} $c 2000 'ordinary OS complete modifier/key acknowledgment'
 [OwnedSettingsInput]::OwnRelease();if($c.ElapsedMilliseconds -ge 2000){throw 'Complete ordinary input exceeded existing2000ms budget'};$script:states+=,$s;return $s
}
function Os-Text([string]$text){
 $c=[Diagnostics.Stopwatch]::StartNew();$b=Settings-State $script:target 2000
 [OwnedSettingsInput]::Text($script:main,$script:target.Slot,$script:demoPid,$text)
 $s=Wait-SettingsState $script:target {param($s)$s.Chars -eq $b.Chars+$text.Length} $c 2000 'ordinary OS character acknowledgment'
 [OwnedSettingsInput]::OwnRelease();if($c.ElapsedMilliseconds -ge 2000){throw 'Complete ordinary input exceeded existing2000ms budget'};$script:states+=,$s;return $s
}
function Check([bool]$value,[string]$message){if(-not $value){throw $message};$script:checks++;Write-Output "PASS $message"}
function Seek([int]$kind,[string]$id,[int]$sub=-1,[int]$direction=1){
 $hash=[FpUi]::SettingsHash($id);$s=Settings-State $script:target 2000
 for($i=0;$i -lt 32;$i++){if($s.FocusKind -eq $kind -and $s.FocusId -eq $hash -and ($sub -lt 0 -or $s.FocusSub -eq $sub)){return $s};$s=Os-Key 9 $(if($direction -lt 0){[ushort[]]@(16)}else{[ushort[]]@()})}
 throw "Logical focus kind=$kind id=$id sub=$sub not reached; $s"
}
function Seed-Clipboard([string]$text){
 Assert-SettingsTarget $script:target
 [OwnedSettingsInput]::Seed($script:main,$text)
 $o=[OwnedSettingsInput]::Observe($script:main,$text)
 Assert-SettingsTarget $script:target
 if(-not $o.Accepted($script:main.ToInt64(),$o.Sequence,0,3000,$false)){throw 'Fixture seed is not an opened/stable exact main-owned observation; no editor clipboard action submitted'}
 return $o.Sequence
}
function Read-EditorPublication([string]$expected,[uint32]$baseline,$clock,[string]$message){
 $publisher=$script:target.Helper.ToInt64();$accepted=$false;$o=$null
 do{
  if($clock.ElapsedMilliseconds -ge 3000){break}
  Assert-SettingsTarget $script:target
  $o=[OwnedSettingsInput]::Observe($script:main,$expected) # one nonblocking read
  Assert-SettingsTarget $script:target
  if($o.Accepted($publisher,$baseline,$clock.ElapsedMilliseconds,3000,$true)){$accepted=$true;break}
  $remaining=3000-[int]$clock.ElapsedMilliseconds
  if($remaining -gt 0){Start-Sleep -Milliseconds ([Math]::Min(5,$remaining))}
 }while($clock.ElapsedMilliseconds -lt 3000)
 Assert-SettingsTarget $script:target
 # Recheck the same retained observation after identity checks, including time.
 $accepted=$accepted -and $o.Accepted($publisher,$baseline,$clock.ElapsedMilliseconds,3000,$true)
 $script:observations+=,@{observation=$o;expected_publisher=$publisher;helper_pid=$script:target.Pid;helper_started=$script:target.Started;baseline=$baseline;elapsed_ms=$clock.ElapsedMilliseconds;budget_ms=3000;accepted=$accepted}
 Check $accepted $message
}
function Copy-Draft([string]$expected){
 $null=Os-Key 65 ([ushort[]]@(17));$sequence=Seed-Clipboard 'owned-copy-sentinel'
 Assert-SettingsTarget $script:target
 $clock=[Diagnostics.Stopwatch]::StartNew();$null=Os-Key 67 ([ushort[]]@(17))
 Read-EditorPublication $expected $sequence $clock 'normal Copy published exact stable helper-owned draft within3000ms'
}

function Open-Field([string]$query,[int]$which=0){
 $null=Os-Text $query;$s=Seek 11 'preview.pdf'
 for($i=0;$i -lt $which;$i++){$null=Os-Key 9;$s=Seek 11 'preview.pdf'}
 $null=Os-Key 13;return (Wait-SettingsState $script:target {param($s)$s.FieldOpen -eq 1 -and $s.Pending -eq 0} ([Diagnostics.Stopwatch]::StartNew()) 2000 'actual ordinary numeric editor')
}
if(-not $EvidenceRoot){$EvidenceRoot=Join-Path $root "build\settings_ui\$([Guid]::NewGuid().ToString('N'))"}
New-Item -ItemType Directory -Path $EvidenceRoot -ErrorAction Stop|Out-Null
[OwnedSettingsInput]::InputFactsFile=Join-Path $EvidenceRoot 'os-input-facts.jsonl'
$left=Join-Path $EvidenceRoot 'left';$right=Join-Path $EvidenceRoot 'right';New-Item -ItemType Directory $left,$right|Out-Null
$ini=Join-Path $EvidenceRoot 'settings.ini'
$base="; owned Settings acceptance`n[functions]`npreview.image = off`npreview.text = off`noperations.show = off`nsvc.icons = off`n[panel.side]`nslide = off`n"
$initial=$base+"[preview.pdf]`nlinger = 7`n[unrelated.r04]`nsentinel = keep`n"
if($Case -like 'N10-*'){$initial=$base+"[relay.r04_a]`ntitle = R04 original`nprogram = cmd`narguments = /d /c exit 0`n"}
if($Case -like 'N12-*'){$initial=$base+"[preview.pdf]`nlinger = 5`nremember = 5`n[unrelated.r04]`nsentinel = keep`n"}
Set-Content $ini -NoNewline $initial
$script:states=@();$script:observations=@();$script:checks=0;$custody=$null;$holder=$null
$p=$null;$target=$null;$activation=$null;$ok=$false;$errorType='';$errorMessage='';$closed=$false;$watch=[Diagnostics.Stopwatch]::StartNew()
try {
 $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$ini",$left,$right)
 $gear=[OwnedSettingsInput]::Gear($script:main,$script:demoPid)
 $x=$gear[0];$y=$gear[1];$xy=($x-band 65535)-bor(($y-band 65535)-shl16)
 [FpUi]::OwnedWindow($script:main,$script:demoPid)
 $null=[FpUi]::Input($script:main,0x201,1,[IntPtr]$xy,1000)
 $null=[FpUi]::Input($script:main,0x202,0,[IntPtr]$xy,1000)
 if(-not(Wait-Until {try{$script:settingsTarget=New-SettingsTarget;return $true}catch{return $false}} 5000)){throw 'Owned Settings helper did not become ready'}
 $target=$script:settingsTarget;$script:target=$target;$state=Wait-SettingsState $target {param($s)$s.Snapshot -gt 0 -and $s.Have -eq 1 -and $s.Pending -eq 0 -and $s.Controls -ge 10} ([Diagnostics.Stopwatch]::StartNew()) 3000 'coherent laid-out initial Settings' 
 if(-not $state.Snapshot -or -not $state.Have -or $state.Pending -or $state.Controls -lt 10){throw 'Coherent Settings snapshot/control prerequisite unavailable'}
 $activation=[OwnedSettingsInput]::Activate($script:main,$target.Slot,$script:demoPid,"$EvidenceRoot\settings-before.bmp","$EvidenceRoot\before-input-facts.jsonl")
 $focused=[OwnedSettingsInput]::GetForegroundWindow() -eq $script:main -and [OwnedSettingsInput]::Focus($script:main,$script:demoPid) -eq $target.Slot.ToInt64()
 if($activation.PointerRestore -ne 'restored'){throw 'Pointer changed or input remained busy; fixture restoration not established'}
 if(-not $focused){throw 'Ordinary click did not establish owned foreground and actual slot focus; no keyboard input delivered'}
 $after=Settings-State $target 3000
 $activation.After=[OwnedSettingsInput]::Probe($script:main,$target.Slot,$script:demoPid,$activation.Before.Client.X,$activation.Before.Client.Y)
 if($Case -eq 'N08'){
  $null=Os-Text 'preview.pdf colors';$choice=Seek 10 'preview.pdf' 2
  $frame=[OwnedSettingsInput]::Capture($script:main,$target.Slot,$script:demoPid,"$EvidenceRoot\original-choice.bmp");$bounds=[OwnedSettingsInput]::Ring($frame)
  $null=Seek 11 'preview.pdf' -1 -1;$null=Os-Key 13;$before=Settings-State $target 2000;Check ($before.FieldOpen -eq 1) 'unchanged linger7 editor open'
  $clock=[Diagnostics.Stopwatch]::StartNew();Check ([OwnedSettingsInput]::Click($script:main,$target.Slot,$script:demoPid,$bounds[0],$bounds[1],"$EvidenceRoot\feature-facts.jsonl") -eq 'restored') 'Original actual click and pointer restored'
  $after=Wait-SettingsSave $target $before 1 $clock 3000;$script:states+=,$after
  Check ($after.Request -eq $before.Request+1 -and $after.Snapshot -gt $before.Snapshot -and $after.FocusKind -eq 10 -and $after.FocusSub -eq 2 -and $after.FocusId -eq [FpUi]::SettingsHash('preview.pdf') -and -not $after.FieldOpen) 'only Original logical choice saved and acknowledged'
  $expected=$initial.Replace("linger = 7`n","linger = 7`ncolors = original`n")
  Check (((Get-Content $ini -Raw)-replace "`r",'') -ceq $expected) 'exact intended private INI delta'
 }elseif($Case -like 'N10-*'){
  $null=Os-Text 'relay.r04_a';$null=Seek 3 'relay.r04_a';$null=Os-Key 13
  $before=Wait-SettingsState $target {param($s)$s.Recording -eq 1 -and $s.Pending -eq 0} ([Diagnostics.Stopwatch]::StartNew()) 2000 'actual shortcut recording'
  $expected=$initial.Replace('r04_a','r04_b').Replace('R04 original','R04 replacement')
  if($Case -eq 'N10-count'){$expected+="[relay.r04_c]`ntitle = R04 added`nprogram = cmd`narguments = /d /c exit 0`n"}
  Set-Content $ini -NoNewline $expected
  $after=Wait-SettingsState $target {param($s)$s.Snapshot -gt $before.Snapshot -and $s.Recording -eq 0 -and $s.Pending -eq 0} ([Diagnostics.Stopwatch]::StartNew()) 3000 'accepted reload cancels recorder'
  $script:states+=,$before;$script:states+=,$after
  Check ($after.Request -eq $before.Request -and $after.Completed -eq $before.Completed) 'reload emits no shortcut assignment'
  # NO activation/click/focus operation after reload. Normal OS accelerator path.
  $null=[OwnedSettingsInput]::Key($script:main,$target.Slot,$script:demoPid,0xBC,[ushort[]]@(17))
  Check (Wait-Until {-not(Get-Process -Id $target.Pid -ErrorAction SilentlyContinue)} 3000) 'normal host accelerator works after reload without focus workaround'
  Check (((Get-Content $ini -Raw)-replace "`r",'') -ceq $expected) 'replacement functions unchanged by stopped recorder'
 }elseif($Case -eq 'N09'){
  $custody=[OwnedSettingsInput+ClipboardCustody]::new($script:main);$before=Open-Field 'preview.pdf linger'
  $seq=Seed-Clipboard 'owned-prior';$holder=[OwnedSettingsInput+ClipboardHold]::new();$cutClock=[Diagnostics.Stopwatch]::StartNew()
  try{$null=Os-Key 88 ([ushort[]]@(17))}finally{$holder.Dispose();$holder=$null}
  Assert-SettingsTarget $script:target;$prior=[OwnedSettingsInput]::Observe($script:main,'owned-prior');Assert-SettingsTarget $script:target
  $priorOk=$prior.Accepted($script:main.ToInt64(),$seq,$cutClock.ElapsedMilliseconds,3000,$false) -and $prior.Sequence -eq $seq
  $script:observations+=,@{observation=$prior;expected_publisher=$script:main.ToInt64();baseline=$seq;elapsed_ms=$cutClock.ElapsedMilliseconds;budget_ms=3000;accepted=$priorOk}
  Check $priorOk 'busy Cut retained stable exact main-owned prior clipboard within3000ms'
  # Copy selected text without Ctrl+A: proves busy Cut retained selection too.
  $copyClock=[Diagnostics.Stopwatch]::StartNew();$null=Os-Key 67 ([ushort[]]@(17))
  Read-EditorPublication '7' $seq $copyClock 'busy Cut retained draft and selected7: exact helper-owned Copy within3000ms'
  $seq=Seed-Clipboard 'owned-prior';Assert-SettingsTarget $script:target
  $cutClock=[Diagnostics.Stopwatch]::StartNew();$null=Os-Key 88 ([ushort[]]@(17))
  Read-EditorPublication '7' $seq $cutClock 'successful actual host Cut publishes exact stable helper-owned selected7 within3000ms'
  $null=Os-Key 39;$null=Os-Text '9';Copy-Draft '9'
  $null=Os-Key 27;Check (((Get-Content $ini -Raw)-replace "`r",'') -ceq $initial) 'Cut changes draft only until explicit save'
 }elseif($Case -like 'N12-*'){
  $custody=[OwnedSettingsInput+ClipboardCustody]::new($script:main)
  $null=Os-Text 'preview.pdf';$first=Seek 11 'preview.pdf';$frame=[OwnedSettingsInput]::Capture($script:main,$target.Slot,$script:demoPid,"$EvidenceRoot\first-field.bmp");$firstBounds=[OwnedSettingsInput]::Ring($frame)
  $null=Os-Key 9;$second=Seek 11 'preview.pdf';$frame=[OwnedSettingsInput]::Capture($script:main,$target.Slot,$script:demoPid,"$EvidenceRoot\second-field.bmp");$secondBounds=[OwnedSettingsInput]::Ring($frame)
  Check ($secondBounds[1] -gt $firstBounds[1]) 'two distinct same-record numeric fields identified'
  $null=Os-Key 9 ([ushort[]]@(16));$null=Seek 11 'preview.pdf' -1 -1;$null=Os-Key 13;$before=Settings-State $target 2000;Check ($before.FieldOpen -eq 1) 'first same-text field opened'
  [OwnedSettingsInput]::Seed($script:main,'9');$holder=[OwnedSettingsInput+ClipboardHold]::new();$pasteClock=[Diagnostics.Stopwatch]::StartNew()
  try{
   $null=Os-Key 86 ([ushort[]]@(17))
   if($Case -eq 'N12-switch'){
    Check ([OwnedSettingsInput]::Click($script:main,$target.Slot,$script:demoPid,$secondBounds[0],$secondBounds[1],"$EvidenceRoot\switch-facts.jsonl") -eq 'restored') 'direct same-text other-editor click'
    $null=Wait-SettingsState $target {param($s)$s.Clicks -gt $before.Clicks -and $s.FieldOpen -eq 1} ([Diagnostics.Stopwatch]::StartNew()) 2000 'other field opened'
   }elseif($Case -eq 'N12-reopen'){$null=Os-Key 27;$null=Os-Key 13}
   elseif($Case -eq 'N12-reload'){
    $expected=$initial.Replace('sentinel = keep','sentinel = reloaded');Set-Content $ini -NoNewline $expected
    $null=Wait-SettingsState $target {param($s)$s.Snapshot -gt $before.Snapshot -and $s.Pending -eq 0} ([Diagnostics.Stopwatch]::StartNew()) 3000 'relevant accepted snapshot'
   }
   Check ($pasteClock.ElapsedMilliseconds -lt 1500) 'clipboard released inside original paste deadline'
  }finally{$holder.Dispose();$holder=$null}
  Start-Sleep -Milliseconds 200
  $current=Settings-State $target 2000;if(-not $current.FieldOpen){$null=Seek 11 'preview.pdf';$null=Os-Key 13}
  Copy-Draft $(if($Case -eq 'N12-same'){'9'}else{'5'})
  $after=Settings-State $target 2000;Check ($after.Request -eq $before.Request -and $after.Completed -eq $before.Completed) 'paste lifecycle creates no unrequested save'
  $null=Os-Key 27
  $expected=if($Case -eq 'N12-reload'){$initial.Replace('sentinel = keep','sentinel = reloaded')}else{$initial}
  Check (((Get-Content $ini -Raw)-replace "`r",'') -ceq $expected) 'only intended external reload changed private INI'
 }
 $ok=$true
}catch{$errorType=$_.Exception.GetType().FullName;$errorMessage=$_.Exception.Message;throw}finally{
 $cleanupErrors=@();if($holder){try{$holder.Dispose()}catch{$cleanupErrors+=$_.Exception.Message}};if($custody){try{$custody.Dispose()}catch{$cleanupErrors+=$_.Exception.Message}};if($p){try{$closed=Stop-Demo $p}catch{$cleanupErrors+=$_.Exception.Message}}
 @{outcome=$(if($ok -and $closed -and -not $cleanupErrors){'passed'}else{'failed'});scope=$Case;cleanup_errors=$cleanupErrors;assertions=$script:checks;states=$script:states;clipboard_observations=$script:observations;clipboard_restored=$(if($custody){$custody.Restored}else{$null});activation=$activation;initial_state=$state;after_state=$after;error_type=$errorType;error=$errorMessage;owned_host_closed=$closed;wall_ms=$watch.Elapsed.TotalMilliseconds;os_keyboard_actions=$(if($Case -eq 'prerequisite'){0}else{'see acknowledged states'});retries=0;skips=0}|ConvertTo-Json -Depth 10|Set-Content "$EvidenceRoot\prerequisite-result.json"
}
if(-not $ok -or -not $closed -or $cleanupErrors){exit 1}
