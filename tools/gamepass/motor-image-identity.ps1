# Prints the in-memory identity of a Motor executable: PE timestamp, SizeOfImage and the code-image SHA-256
# that build_profile.cpp computes for builds whose file cannot be read (Microsoft Store / Game Pass).
# Code-image SHA-256: the .text section as mapped (VirtualSize bytes) with every base-relocated pointer
# inside it zeroed, so the value is independent of the load address.
#   -ProcessName TheGreatCircle   read the running game (read-only)
#   -ImageFile <path>             read an image laid out by virtual address (tools/gamepass/dump-motor-image.ps1 output)
[CmdletBinding(DefaultParameterSetName='process')]
param([Parameter(ParameterSetName='process')][string]$ProcessName = 'TheGreatCircle',
      [Parameter(ParameterSetName='file',Mandatory)][string]$ImageFile)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MotorImageIdentity {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("psapi.dll", SetLastError=true)] static extern bool EnumProcessModulesEx(IntPtr h, IntPtr[] mods, int cb, out int needed, uint filter);
    public static byte[] ReadImage(int pid) {
        IntPtr h = OpenProcess(0x0410, false, pid);
        if (h == IntPtr.Zero) throw new Exception("OpenProcess failed: " + Marshal.GetLastWin32Error());
        try {
            var mods = new IntPtr[1]; int needed; IntPtr got;
            if (!EnumProcessModulesEx(h, mods, IntPtr.Size, out needed, 3)) throw new Exception("EnumProcessModulesEx failed");
            var hdr = new byte[0x1000];
            if (!ReadProcessMemory(h, mods[0], hdr, (IntPtr)hdr.Length, out got)) throw new Exception("header read failed");
            int pe = BitConverter.ToInt32(hdr, 0x3c);
            var image = new byte[BitConverter.ToUInt32(hdr, pe + 24 + 56)];
            var page = new byte[0x1000];
            for (long off = 0; off < image.Length; off += 0x1000)
                if (ReadProcessMemory(h, (IntPtr)(mods[0].ToInt64() + off), page, (IntPtr)0x1000, out got) && got.ToInt64() == 0x1000)
                    Buffer.BlockCopy(page, 0, image, (int)off, 0x1000);
            return image;
        } finally { CloseHandle(h); }
    }
    public static string Describe(byte[] img) {
        int pe = BitConverter.ToInt32(img, 0x3c), opt = pe + 24;
        uint stamp = BitConverter.ToUInt32(img, pe + 8), size = BitConverter.ToUInt32(img, opt + 56);
        int sections = BitConverter.ToUInt16(img, pe + 6), table = opt + BitConverter.ToUInt16(img, pe + 20);
        int lo = -1, len = 0;
        for (int i = 0; i < sections; i++) {
            int s = table + i * 40;
            if (System.Text.Encoding.ASCII.GetString(img, s, 8).TrimEnd('\0') == ".text") { len = BitConverter.ToInt32(img, s + 8); lo = BitConverter.ToInt32(img, s + 12); }
        }
        if (lo < 0) throw new Exception("no .text section");
        var code = new byte[len]; Buffer.BlockCopy(img, lo, code, 0, len);
        uint relocVa = BitConverter.ToUInt32(img, opt + 112 + 5 * 8), relocSize = BitConverter.ToUInt32(img, opt + 112 + 5 * 8 + 4);
        long zeroed = 0;
        for (uint b = relocVa; b + 8 <= relocVa + relocSize; ) {
            uint pageVa = BitConverter.ToUInt32(img, (int)b), blockSize = BitConverter.ToUInt32(img, (int)b + 4);
            if (blockSize < 8) break;
            for (uint e = b + 8; e + 2 <= b + blockSize; e += 2) {
                int entry = BitConverter.ToUInt16(img, (int)e), type = entry >> 12, width = type == 10 ? 8 : type == 3 ? 4 : 0;
                long at = pageVa + (entry & 0xfff);
                for (int k = 0; k < width; k++) { long p = at + k - lo; if (p >= 0 && p < len) { code[p] = 0; zeroed++; } }
            }
            b += blockSize;
        }
        string hash;
        using (var sha = System.Security.Cryptography.SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(code)).Replace("-", "").ToLowerInvariant();
        return String.Format("timestamp 0x{0:x}\nimageSize 0x{1:x}\ncodeImageSha256 {2}\n(.text 0x{3:x}+0x{4:x}, {5} relocated bytes zeroed)", stamp, size, hash, lo, len, zeroed);
    }
}
'@
if ($PSCmdlet.ParameterSetName -eq 'file') { [MotorImageIdentity]::Describe([IO.File]::ReadAllBytes($ImageFile)) }
else {
    $p = @(Get-Process -Name $ProcessName -ErrorAction Stop)
    if ($p.Count -ne 1) { throw "Expected one $ProcessName process, found $($p.Count)." }
    [MotorImageIdentity]::Describe([MotorImageIdentity]::ReadImage($p[0].Id))
}
