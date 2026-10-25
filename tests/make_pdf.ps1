# make_pdf.ps1 - writes a plain text PDF for tests: US Letter pages with a
# heading, justified paragraphs inside 1-inch margins and a page number at
# the bottom. No dependencies. Body lines are 78 characters of 10-pt
# Courier (6 pt each) from x = 72, so the text box is exactly 72..540 pt;
# line k of a page has its baseline 102 + 14 k pt from the top, with every
# 9th line left empty between paragraphs.
param(
    [Parameter(Mandatory)] [string] $Out,
    [int] $Pages = 6,
    [string] $Heading = 'Chapter',
    [string] $Swatch = ''
)
$ErrorActionPreference = 'Stop'

$prose = 'the quick brown fox jumps over a lazy dog while seven bright owls watch from an old oak tree near the quiet river bank and count the stars one by one '
$script:at = 0
function Next-Line {
    # the next 78 characters of the running text, never ending in a space
    $t = ''
    while ($t.Length -lt 78) { $t += $prose[$script:at % $prose.Length]; $script:at++ }
    if ($t[0] -eq ' ') { $t = 'x' + $t.Substring(1) }
    if ($t[77] -eq ' ') { $t = $t.Substring(0, 77) + 'x' }
    $t
}
function Body([int] $page) {
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append("BT /F2 18 Tf 72 720 Td ($Heading $page) Tj ET`n")
    if($Swatch){[void]$sb.Append("$Swatch rg 540 720 40 30 re f`n")}
    $y = 690
    for ($line = 0; $line -lt 44; $line++) {
        if ($line % 9 -eq 8) { $y -= 14; continue } # paragraph break
        [void]$sb.Append("BT /F1 10 Tf 72 $y Td ($(Next-Line)) Tj ET`n")
        $y -= 14
    }
    [void]$sb.Append("BT /F1 9 Tf 300 40 Td ($page) Tj ET`n")
    $sb.ToString()
}

$objs = New-Object Collections.Generic.List[string]
$kids = (0..($Pages - 1) | ForEach-Object { "$(4 + 2 * $_) 0 R" }) -join ' '
$objs.Add("<< /Type /Catalog /Pages 2 0 R >>")
$objs.Add("<< /Type /Pages /Kids [$kids] /Count $Pages >>")
$objs.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>")
for ($i = 0; $i -lt $Pages; $i++) {
    $content = Body ($i + 1)
    $objs.Add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R /F2 $(4 + 2 * $Pages) 0 R >> >> /Contents $(5 + 2 * $i) 0 R >>")
    $objs.Add("<< /Length $($content.Length) >>`nstream`n$content`nendstream")
}
$objs.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")

$enc = [Text.Encoding]::ASCII
$sb = New-Object Text.StringBuilder
[void]$sb.Append("%PDF-1.4`n")
$offs = @()
for ($i = 0; $i -lt $objs.Count; $i++) {
    $offs += $enc.GetByteCount($sb.ToString())
    [void]$sb.Append("$($i + 1) 0 obj`n$($objs[$i])`nendobj`n")
}
$xref = $enc.GetByteCount($sb.ToString())
[void]$sb.Append("xref`n0 $($objs.Count + 1)`n0000000000 65535 f `n")
foreach ($o in $offs) { [void]$sb.Append(('{0:D10} 00000 n ' -f $o) + "`n") }
[void]$sb.Append("trailer`n<< /Size $($objs.Count + 1) /Root 1 0 R >>`nstartxref`n$xref`n%%EOF`n")
[IO.File]::WriteAllBytes($Out, $enc.GetBytes($sb.ToString()))
