<# Runs an off-screen owned WebView2 host. Optional private files remain local. #>
param([string]$BuildDirectory, [string]$Document, [int]$ExpectedMath = 2, [int]$ExpectedErrors = 0, [switch]$ArchiveLockedPath,
      [ValidateSet('markdown','json','jsonl','csv','tsv','notebook','yaml','toml','ini','xml','xaml','rss','atom')][string]$Format='markdown')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
$helper=Join-Path $BuildDirectory $(if($Format -eq 'markdown'){'folderwing-markdown.exe'}elseif($Format -eq 'notebook'){'folderwing-notebook.exe'}elseif($Format -in @('xml','xaml','rss','atom')){'folderwing-xml.exe'}elseif($Format -in @('yaml','toml','ini')){'folderwing-config.exe'}elseif($Format -in @('csv','tsv')){'folderwing-table.exe'}else{'folderwing-json.exe'})
if(!(Test-Path $helper)){throw 'Build the Markdown helper first'}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-markdown-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
$lock=$null
try {
 if(!$Document){
   if($Format -in @('xml','xaml','rss','atom')){$Document=Join-Path $scratch ("sample."+$Format);$sample='<?xml version="1.0"?><root xmlns="urn:test"><item a="1">hello<![CDATA[ world]]></item><!-- comment --></root>';if(!$PSBoundParameters.ContainsKey('ExpectedMath')){$ExpectedMath=1}}
   elseif($Format -eq 'json'){$Document=Join-Path $scratch 'sample.json';$sample='{"big":900719925474099312345,"values":[true,null,"text"]}';if(!$PSBoundParameters.ContainsKey('ExpectedMath')){$ExpectedMath=1}}
   elseif($Format -eq 'jsonl'){$Document=Join-Path $scratch 'sample.jsonl';$sample=((1..70|ForEach-Object {if($_ -eq 2){'invalid'}else{'{"big":900719925474099312345}'}}) -join "`n");if(!$PSBoundParameters.ContainsKey('ExpectedMath')){$ExpectedMath=64}}
   elseif($Format -eq 'csv'){$Document=Join-Path $scratch 'sample.csv';$sample="name,value`r`n`"a,b`",007`r`n`"multi`r`nline`",2`r`n";if(!$PSBoundParameters.ContainsKey('ExpectedMath')){$ExpectedMath=3}}
   elseif($Format -eq 'tsv'){$Document=Join-Path $scratch 'sample.tsv';$sample="name`tvalue`n`"a`tb`"`t007`n";if(!$PSBoundParameters.ContainsKey('ExpectedMath')){$ExpectedMath=2}}
   elseif($Format -eq 'notebook'){$Document=Join-Path $scratch 'sample.ipynb';$sample='{"nbformat":4,"metadata":{"language_info":{"name":"python"}},"cells":[{"cell_type":"markdown","source":["# Heading\n","\\[\nx^2\n\\]\n"]},{"cell_type":"code","execution_count":1,"source":["print(1)\n"],"outputs":[{"output_type":"stream","name":"stdout","text":["1\n"]},{"output_type":"display_data","data":{"image/png":"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="},"metadata":{}}]}]}';if(!$PSBoundParameters.ContainsKey('ExpectedMath')){$ExpectedMath=2}}
   elseif($Format -eq 'yaml'){$Document=Join-Path $scratch 'sample.yaml';$sample="defaults: &defaults`n  big: 9223372036854775807`ncopy: *defaults`n---`ncustom: !widget hello`n";if(!$PSBoundParameters.ContainsKey('ExpectedMath')){$ExpectedMath=2}}
   elseif($Format -eq 'toml'){$Document=Join-Path $scratch 'sample.toml';$sample="big=9223372036854775807`nwhen=1979-05-27T07:32:00Z`n[owner]`nname=`"Rose`"`n";if(!$PSBoundParameters.ContainsKey('ExpectedMath')){$ExpectedMath=1}}
   elseif($Format -eq 'ini'){$Document=Join-Path $scratch 'sample.ini';$sample="root=007`r`n[General]`r`nkey=first`r`nkey=second`r`nunknown line`r`n[General]`r`nkey=third`r`n";if(!$PSBoundParameters.ContainsKey('ExpectedMath')){$ExpectedMath=1}}
   else{$Document=Join-Path $scratch 'sample.md';$sample="# Math`n`n\[`n\mathrm{inj}_{x} + \mathrm{match}_{y}`n\]`n`nInline \(x^2\) and citation \[User Query\]."}
   [IO.File]::WriteAllText($Document,$sample,[Text.UTF8Encoding]::new($false))
 }
 if($ArchiveLockedPath){
   if($Document -notlike "$scratch\*"){throw 'The archive-lock fixture must be an owned test file'}
   $lock=[IO.File]::Open($Document,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
   $Document='\\?\'+$Document
 }
 $exe=Join-Path $scratch 'probe.exe'
 $csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll (Join-Path $root 'tests\markdown_host_smoke.cs')
 if($LASTEXITCODE){throw 'Markdown test host compilation failed'}
 & $exe $helper $Document $ExpectedMath $ExpectedErrors
 if($LASTEXITCODE){throw 'Markdown host acceptance failed'}
} finally {if($lock){$lock.Dispose()};Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
