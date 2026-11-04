<#
  Game Pass build column

  The Microsoft Store / Game Pass executable is encrypted on disk, so it cannot be hashed or diffed as a
  file. Its column in profiles/motor-builds.json is produced from the running game instead, with the
  Steam executable (an exact catalogued build) as the reference. Run every step with Windows PowerShell.

  1. Start the Game Pass game without the mod, then dump its mapped image (read-only):
     .\dump-motor-image.ps1 -Out <work>\gamepass.exe
  2. Identity for the profile (timestamp, image size, code-image SHA-256 checked by build_profile.cpp):
     .\motor-image-identity.ps1 -ImageFile <work>\gamepass.exe
  3. Anchor search from the catalog's own 32-byte samples:
     .\probe-gamepass-code.ps1 -Catalog ..\..\profiles\motor-builds.json -Dump <work>\gamepass.exe -Out <work>\probe.json
  4. Map every address through the Steam executable: code by 128 bytes of surrounding Steam code; data
     through every RIP-relative reference to it in Steam code, located again in Game Pass code, with votes;
     template initialisers placed by the nearest exact code anchor; the rest bracketed by neighbours that
     moved by the same amount and confirmed by a Game Pass reference that lands on the predicted address.
     .\map-gamepass-build.ps1 -Catalog ..\..\profiles\motor-builds.json -SteamExe <steam>\TheGreatCircle.exe -Dump <work>\gamepass.exe -CodeProbe <work>\probe.json -Out <work>\mapping.json
  5. Review what step 4 leaves unmapped. For 1.0.17.0 three entries were added by hand to mapping.json:
     - 0x4c52390 -> 0x4c3a510 and 0x4c523a8 -> 0x4c3a528 (render frame struct and its token): the only
       Steam reference sits in code that differs in Game Pass; every mapped reference from Steam
       0x4c94f71 to 0x4c9d960 shifts by -0x5af00, and the dump holds the two frame counters at +0/+8.
     - 0xeb6a77 -> 0xea3e57 (menu cursor poll): the sample with its struct field masked; Game Pass
       stores the field at +0x1da68 instead of +0x1da70. The hook itself does not use that field.
     0x44f37e0 stays unmapped (null); no source file uses it.
  6. Write the column and regenerate the native table:
     python import-gamepass-build.py --mapping <work>\mapping.json --image <work>\gamepass.exe --code-image-sha256 <hash> --timestamp <stamp> --image-size <size> --package-version <package>
     python ..\generate-build-profiles.py then ..\check-build-profiles.ps1

  Never commit or share the dumped image: it is the decrypted game executable.
#>
param([Parameter(Mandatory)][string]$Catalog, [Parameter(Mandatory)][string]$SteamExe, [Parameter(Mandatory)][string]$Dump,
      [Parameter(Mandatory)][string]$CodeProbe, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'MotorXrefMap.cs') -Raw)
$cat = Get-Content -LiteralPath $Catalog -Raw | ConvertFrom-Json
$probe = Get-Content -LiteralPath $CodeProbe -Raw | ConvertFrom-Json
$steam = [XrefMap]::Load($SteamExe); $gp = [XrefMap]::Load($Dump)
"steam .text 0x{0:x}..0x{1:x}; gamepass .text 0x{2:x}..0x{3:x}" -f $steam.TextLo, $steam.TextHi, $gp.TextLo, $gp.TextHi
function HexToL($s) { [Convert]::ToInt64($s, 16) }
$entries = @($cat.addresses.PSObject.Properties | ForEach-Object { [pscustomobject]@{ gog = (HexToL $_.Name); steam = (HexToL $_.Value.steam); code = [bool]$_.Value.code } })

# Code: anchors from the dump-only probe, then every code address located again by 128 bytes of Steam context.
$prev = @{}
foreach ($r in $probe) { $at = if ($r.hits -eq 1) { $r.gamepass } elseif ($r.resolvedByNeighbour) { $r.resolvedByNeighbour } else { $null }; if ($at) { $prev[(HexToL $r.gog)] = (HexToL $at) } }
$codeE = @($entries | Where-Object code)
[XrefMap]::SetAnchors([long[]]@($codeE | Where-Object { $prev.ContainsKey($_.gog) } | ForEach-Object steam), [long[]]@($codeE | Where-Object { $prev.ContainsKey($_.gog) } | ForEach-Object { $prev[$_.gog] }))
$result = @{}; $agree = 0; $disagree = @(); $newCode = @(); $codeMiss = @()
foreach ($e in $codeE) {
    $hits = 0; $at = [XrefMap]::Locate($steam, $gp, $e.steam, 48, 80, [ref]$hits)
    if ($at -lt 0) { $at = [XrefMap]::Locate($steam, $gp, $e.steam, 160, 160, [ref]$hits) }
    if ($at -ge 0) {
        $result[$e.gog] = [pscustomobject]@{ gog = '0x{0:x}' -f $e.gog; steam = '0x{0:x}' -f $e.steam; gamepass = '0x{0:x}' -f $at; code = $true; method = 'steam-context'; votes = 1 }
        if ($prev.ContainsKey($e.gog)) { if ($prev[$e.gog] -eq $at) { $agree++ } else { $disagree += '0x{0:x}: probe 0x{1:x} vs steam 0x{2:x}' -f $e.gog, $prev[$e.gog], $at } } else { $newCode += '0x{0:x}' -f $e.gog }
    } else { $codeMiss += '0x{0:x} (hits {1})' -f $e.gog, $hits }
}
"code: located $($result.Count) of $($codeE.Count); agrees with dump-only probe: $agree; disagrees: $($disagree.Count); newly mapped: $($newCode.Count); still missing: $($codeMiss.Count)"
$disagree | ForEach-Object { "  disagree $_" }; $codeMiss | ForEach-Object { "  missing $_" }
$located = @($result.Values)
[XrefMap]::SetAnchors([long[]]@($located | ForEach-Object { HexToL $_.steam }), [long[]]@($located | ForEach-Object { HexToL $_.gamepass }))

# Data: each RIP-relative reference to the Steam address, found again in Game Pass, gives one vote.
$dataE = @($entries | Where-Object { -not $_.code })
$targets = [Collections.Generic.HashSet[long]]::new(); foreach ($e in $dataE) { [void]$targets.Add($e.steam) }
$xrefs = [XrefMap]::Xrefs($steam, $targets)
"data: $($xrefs.Count) RIP-relative references in Steam .text to $($dataE.Count) data addresses"
$votes = @{}; $siteHits = @()
foreach ($x in $xrefs) {
    $site = $x[0]; $extra = $x[1]; $t = $x[2]; $hits = 0
    $at = [XrefMap]::Locate($steam, $gp, $site, 24, 28, [ref]$hits)
    if ($at -lt 0) { continue }
    $gt = $at + 4 + $extra + [XrefMap]::ReadInt($gp, $at)
    $siteHits += ,@($site, $at, $t, $gt)
    if (-not $votes.ContainsKey($t)) { $votes[$t] = @{} }
    $votes[$t][$gt] = 1 + $(if ($votes[$t].ContainsKey($gt)) { $votes[$t][$gt] } else { 0 })
}
$refCount = @{}; foreach ($x in $xrefs) { $refCount[$x[2]] = 1 + $(if ($refCount.ContainsKey($x[2])) { $refCount[$x[2]] } else { 0 }) }
$noRefs = @(); $noLoc = @(); $split = @(); $mappedData = 0
foreach ($e in $dataE) {
    if (-not $refCount.ContainsKey($e.steam)) { $noRefs += $e; continue }
    if (-not $votes.ContainsKey($e.steam)) { $noLoc += $e; continue }
    $v = $votes[$e.steam].GetEnumerator() | Sort-Object Value -Descending
    $top = @($v)[0]; $rest = (@($v) | Select-Object -Skip 1 | Measure-Object Value -Sum).Sum
    if ($rest -and $top.Value -lt 3 * $rest) { $split += '0x{0:x}: {1}' -f $e.gog, ((@($v) | ForEach-Object { '0x{0:x} x{1}' -f $_.Key, $_.Value }) -join ', '); continue }
    $result[$e.gog] = [pscustomobject]@{ gog = '0x{0:x}' -f $e.gog; steam = '0x{0:x}' -f $e.steam; gamepass = '0x{0:x}' -f $top.Key; code = $false; method = 'steam-xref'; votes = "$($top.Value)/$($refCount[$e.steam])" }
    $mappedData++
}
"data mapped by references: $mappedData; no Steam references: $($noRefs.Count); references not found in Game Pass: $($noLoc.Count); conflicting votes: $($split.Count)"
$split | ForEach-Object { "  conflict $_" }

# Data with no direct reference: same offset from the nearest mapped data address, when GOG and Steam keep that offset.
$mapped = @($result.Values | Where-Object { -not $_.code } | ForEach-Object { [pscustomobject]@{ gog = (HexToL $_.gog); steam = (HexToL $_.steam); gp = (HexToL $_.gamepass) } })
$near = 0
foreach ($e in @($noRefs + $noLoc)) {
    $n = $mapped | Where-Object { ($e.steam - $_.steam) -eq ($e.gog - $_.gog) -and [math]::Abs($e.gog - $_.gog) -le 0x100 } | Sort-Object { [math]::Abs($e.gog - $_.gog) } | Select-Object -First 1
    if ($n) { $result[$e.gog] = [pscustomobject]@{ gog = '0x{0:x}' -f $e.gog; steam = '0x{0:x}' -f $e.steam; gamepass = '0x{0:x}' -f ($n.gp + $e.gog - $n.gog); code = $false; method = 'field-offset'; votes = '0x{0:x}' -f $n.gog }; $near++ }
}
"data mapped as field offsets of a mapped neighbour: $near"

# Second pass: ambiguous reference sites count when the hit sits where the nearest code anchor predicts
# (within 0x80, no rival hit that close) and the target lands near the neighbouring mapped data (+-0x4000).
$mapped = @($result.Values | Where-Object { -not $_.code } | ForEach-Object { [pscustomobject]@{ steam = (HexToL $_.steam); shift = (HexToL $_.gamepass) - (HexToL $_.steam) } } | Sort-Object steam)
# Every placed reference whose target agreed with the accepted result is an exact Steam->GamePass code anchor.
$okT = @{}; foreach ($r in $result.Values) { if (-not $r.code) { $okT[(HexToL $r.steam)] = (HexToL $r.gamepass) } }
$aS = [Collections.Generic.List[long]]::new(); $aG = [Collections.Generic.List[long]]::new()
foreach ($r in $result.Values) { if ($r.code) { $aS.Add((HexToL $r.steam)); $aG.Add((HexToL $r.gamepass)) } }
foreach ($h in $siteHits) { if ($okT.ContainsKey($h[2]) -and $okT[$h[2]] -eq $h[3]) { $aS.Add($h[0]); $aG.Add($h[1]) } }
[XrefMap]::SetAnchors($aS.ToArray(), $aG.ToArray()); "code anchors for second pass: $($aS.Count)"
$byTarget = @{}; foreach ($x in $xrefs) { if (-not $byTarget.ContainsKey($x[2])) { $byTarget[$x[2]] = @() }; $byTarget[$x[2]] += ,$x }
$pass2 = 0; $pass2Notes = @()
foreach ($e in @($dataE | Where-Object { -not $result.ContainsKey($_.gog) -and $byTarget.ContainsKey($_.steam) })) {
    $below = $mapped | Where-Object { $_.steam -lt $e.steam } | Select-Object -Last 1; $above = $mapped | Where-Object { $_.steam -gt $e.steam } | Select-Object -First 1
    $sh = @(@($below, $above) | Where-Object { $_ } | ForEach-Object shift)
    $lo = $e.steam + ($sh | Measure-Object -Minimum).Minimum - 0x4000; $hi = $e.steam + ($sh | Measure-Object -Maximum).Maximum + 0x4000
    $v = @{}
    foreach ($x in $byTarget[$e.steam]) {
        $pred = $x[0] + [XrefMap]::ShiftAt($x[0])
        $hs = @([XrefMap]::LocateAll($steam, $gp, $x[0], 24, 28, 0x80000, 4000) | Sort-Object { [math]::Abs($_ - $pred) })
        if ($hs.Count -eq 0 -or [math]::Abs($hs[0] - $pred) -gt 0x80 -or ($hs.Count -gt 1 -and [math]::Abs($hs[1] - $pred) -le 0x80)) { continue }
        $ts = @($hs[0] | ForEach-Object { $_ + 4 + $x[1] + [XrefMap]::ReadInt($gp, $_) } | Where-Object { $_ -ge $lo -and $_ -le $hi })
        if ($ts.Count -eq 1) { $v[$ts[0]] = 1 + $(if ($v.ContainsKey($ts[0])) { $v[$ts[0]] } else { 0 }) }
    }
    if ($v.Count -eq 1) {
        $k = @($v.Keys)[0]; $pass2++
        $result[$e.gog] = [pscustomobject]@{ gog = '0x{0:x}' -f $e.gog; steam = '0x{0:x}' -f $e.steam; gamepass = '0x{0:x}' -f $k; code = $false; method = 'steam-xref-windowed'; votes = "$($v[$k])/$($byTarget[$e.steam].Count)" }
        $pass2Notes += '  0x{0:x} -> 0x{1:x} ({2}/{3} sites)' -f $e.gog, $k, $v[$k], $byTarget[$e.steam].Count
    } elseif ($v.Count -gt 1) { $pass2Notes += '  0x{0:x} still split: {1}' -f $e.gog, ((@($v.GetEnumerator()) | ForEach-Object { '0x{0:x} x{1}' -f $_.Key, $_.Value }) -join ', ') }
}
"data mapped by windowed references: $pass2"; $pass2Notes

# Code the Steam context could not place keeps the dump-only probe result (its own 32-byte sample matched uniquely).
$fromProbe = 0
foreach ($e in $codeE) { if (-not $result.ContainsKey($e.gog) -and $prev.ContainsKey($e.gog)) { $result[$e.gog] = [pscustomobject]@{ gog = '0x{0:x}' -f $e.gog; steam = '0x{0:x}' -f $e.steam; gamepass = '0x{0:x}' -f $prev[$e.gog]; code = $true; method = 'dump-probe'; votes = 1 }; $fromProbe++ } }
"code kept from dump-only probe: $fromProbe"
$codeLeft = @($codeE | Where-Object { -not $result.ContainsKey($_.gog) }); "code still unmapped: $($codeLeft.Count)"; $codeLeft | ForEach-Object { '  0x{0:x} (steam 0x{1:x})' -f $_.gog, $_.steam }
$left = @($dataE | Where-Object { -not $result.ContainsKey($_.gog) })
"data still unmapped: $($left.Count)"; $left | ForEach-Object { "  0x{0:x} (steam 0x{1:x}, refs {2})" -f $_.gog, $_.steam, $(if ($refCount.ContainsKey($_.steam)) { $refCount[$_.steam] } else { 0 }) }
"TOTAL mapped: $($result.Count) of $($entries.Count)"
$result.Values | Sort-Object { HexToL $_.gog } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $Out -Encoding utf8

# Third pass: when the mapped data on both sides moved by the same amount (and GOG/Steam spacing agrees), the
# address in between moved by it too. Accepted only when some Game Pass copy of a Steam reference lands on it.
$mapped = @($result.Values | Where-Object { -not $_.code } | ForEach-Object { [pscustomobject]@{ gog = (HexToL $_.gog); steam = (HexToL $_.steam); shift = (HexToL $_.gamepass) - (HexToL $_.steam) } } | Sort-Object steam)
$pass3 = 0; $pass3Notes = @()
foreach ($e in @($dataE | Where-Object { -not $result.ContainsKey($_.gog) })) {
    $below = $mapped | Where-Object { $_.steam -lt $e.steam } | Select-Object -Last 1; $above = $mapped | Where-Object { $_.steam -gt $e.steam } | Select-Object -First 1
    if (-not $below -or -not $above -or $below.shift -ne $above.shift -or ($e.steam - $below.steam) -ne ($e.gog - $below.gog) -or ($above.steam - $e.steam) -ne ($above.gog - $e.gog)) {
        $pass3Notes += '  0x{0:x}: neighbours disagree, left unmapped' -f $e.gog; continue }
    $p = $e.steam + $below.shift; $hit = $null
    foreach ($x in @($byTarget[$e.steam])) { if (-not $x) { continue }
        foreach ($ctx in @(@(24, 28), @(12, 12), @(8, 8))) {
            foreach ($h in [XrefMap]::LocateAll($steam, $gp, $x[0], $ctx[0], $ctx[1], 0x80000, 4000)) { if ($h + 4 + $x[1] + [XrefMap]::ReadInt($gp, $h) -eq $p) { $hit = $h; break } }
            if ($hit) { break } }
        if ($hit) { break } }
    if ($hit) { $pass3++; $result[$e.gog] = [pscustomobject]@{ gog = '0x{0:x}' -f $e.gog; steam = '0x{0:x}' -f $e.steam; gamepass = '0x{0:x}' -f $p; code = $false; method = 'bracketed+xref'; votes = '0x{0:x}' -f $hit }; $pass3Notes += '  0x{0:x} -> 0x{1:x} (confirmed by GP reference at 0x{2:x})' -f $e.gog, $p, $hit }
    else { $pass3Notes += '  0x{0:x}: predicted 0x{1:x}, no Game Pass reference confirms it' -f $e.gog, $p }
}
"data mapped by bracketing + confirming reference: $pass3"; $pass3Notes
"FINAL mapped: $($result.Count) of $($entries.Count)"
$result.Values | Sort-Object { HexToL $_.gog } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $Out -Encoding utf8
