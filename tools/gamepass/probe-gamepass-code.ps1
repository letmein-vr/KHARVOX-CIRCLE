param([Parameter(Mandatory)][string]$Catalog, [Parameter(Mandatory)][string]$Dump, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
public static class Probe {
    // Bytes that differ between the GOG and Steam samples are relocated operands. Rel32 calls/jumps and
    // RIP-relative operands can also match by chance in those two builds, so mask those by shape too.
    public static bool[] Mask(byte[] gog, byte[] steam, bool extended) {
        var m = new bool[32];
        for (int i = 0; i < 32; i++) m[i] = gog[i] != steam[i];
        if (!extended) return m;
        for (int i = 0; i < 32; i++) {
            if ((gog[i] == 0xe8 || gog[i] == 0xe9) && i + 4 < 32) for (int k = 1; k <= 4; k++) m[i + k] = true;
            if (i + 5 < 32 && (gog[i + 1] & 0xC7) == 0x05 && Array.IndexOf(new byte[]{0x8b,0x8d,0x89,0x3b,0x39,0x80,0x83,0xff,0xc7,0x10,0x11,0x28,0x29,0xc6,0x38,0x88,0x63,0xb6,0xb7,0x2e,0x2f}, gog[i]) >= 0)
                for (int k = 2; k <= 5; k++) m[i + k] = true;
        }
        return m;
    }
    public static List<int> Find(byte[] img, int lo, int hi, byte[] pat, bool[] mask) {
        var hits = new List<int>();
        int a = 0; while (a < 32 && mask[a]) a++;
        if (a == 32) return hits;
        byte first = pat[a];
        for (int p = lo + a; p < hi - 32 + a; p++) {
            p = Array.IndexOf(img, first, p, hi - 32 + a - p);
            if (p < 0) break;
            int s = p - a; bool ok = true;
            for (int i = 0; i < 32; i++) if (!mask[i] && img[s + i] != pat[i]) { ok = false; break; }
            if (ok) { hits.Add(s); if (hits.Count > 8) break; }
        }
        return hits;
    }
}
'@
$cat = Get-Content -LiteralPath $Catalog -Raw | ConvertFrom-Json
$img = [IO.File]::ReadAllBytes($Dump)
# .text bounds from the image's own section table.
$pe = [BitConverter]::ToInt32($img, 0x3c); $table = $pe + 24 + [BitConverter]::ToUInt16($img, $pe + 20)
$text = 0..([BitConverter]::ToUInt16($img, $pe + 6) - 1) | ForEach-Object { $table + $_ * 40 } | Where-Object { [Text.Encoding]::ASCII.GetString($img, $_, 5) -eq ".text" } | Select-Object -First 1
$textLo = [BitConverter]::ToInt32($img, $text + 12); $textHi = $textLo + [BitConverter]::ToInt32($img, $text + 8)
function Hex([string]$h) { [byte[]](0..31 | ForEach-Object { [Convert]::ToByte($h.Substring($_ * 2, 2), 16) }) }
$rows = @()
foreach ($p in $cat.addresses.PSObject.Properties) {
    $a = $p.Value; if (-not $a.code) { continue }
    $gog = Hex $a.gogBytes; $steam = Hex $a.steamBytes
    $hits = [Probe]::Find($img, $textLo, $textHi, $gog, [Probe]::Mask($gog, $steam, $false))
    $mode = 'strict'
    if ($hits.Count -eq 0) { $hits = [Probe]::Find($img, $textLo, $textHi, $gog, [Probe]::Mask($gog, $steam, $true)); $mode = 'extended' }
    $rows += [pscustomobject]@{ gog = $p.Name; steam = $a.steam; mode = $mode; hits = $hits.Count; gamepass = ($hits | ForEach-Object { '0x{0:x}' -f $_ }) -join ' ' }
}
# Unique hits give the local GOG->Game Pass shift; an ambiguous sample is accepted when exactly one of
# its hits lies within 64 KB of the shift its unique neighbours predict.
$unique = @($rows | Where-Object hits -eq 1)
foreach ($r in $rows | Where-Object { $_.hits -gt 1 }) {
    $g = [Convert]::ToInt64($r.gog, 16)
    $near = $unique | Sort-Object { [math]::Abs([Convert]::ToInt64($_.gog, 16) - $g) } | Select-Object -First 1
    $shift = [Convert]::ToInt64($near.gamepass, 16) - [Convert]::ToInt64($near.gog, 16)
    $close = @($r.gamepass -split ' ' | Where-Object { [math]::Abs([Convert]::ToInt64($_, 16) - ($g + $shift)) -lt 0x10000 })
    $r | Add-Member resolvedByNeighbour ($(if ($close.Count -eq 1) { $close[0] } else { '' }))
}
$rows | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $Out -Encoding utf8
$byNeighbour = @($rows | Where-Object { $_.resolvedByNeighbour }).Count
"code addresses: $($rows.Count)"
"unique match:   $($unique.Count) (strict $(@($unique | Where-Object mode -eq 'strict').Count), extended $(@($unique | Where-Object mode -eq 'extended').Count))"
"ambiguous:      $(@($rows | Where-Object { $_.hits -gt 1 }).Count) (resolved by neighbour shift: $byNeighbour)"
"not found:      $(@($rows | Where-Object hits -eq 0).Count)"
$shifts = $unique | ForEach-Object { [Convert]::ToInt64($_.gamepass, 16) - [Convert]::ToInt64($_.gog, 16) }
"GOG->GamePass shift range: {0} .. {1}" -f ("0x{0:x}" -f [int64]($shifts | Measure-Object -Minimum).Minimum), ("0x{0:x}" -f [int64]($shifts | Measure-Object -Maximum).Maximum)
