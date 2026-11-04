param([string]$ProcessName = 'TheGreatCircle', [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class ImageDump {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("psapi.dll", SetLastError=true)] static extern bool EnumProcessModulesEx(IntPtr h, IntPtr[] mods, int cb, out int needed, uint filter);

    public static string Dump(int pid, string outPath) {
        IntPtr h = OpenProcess(0x0410, false, pid); // PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
        if (h == IntPtr.Zero) throw new Exception("OpenProcess failed: " + Marshal.GetLastWin32Error());
        try {
            var mods = new IntPtr[1]; int needed;
            if (!EnumProcessModulesEx(h, mods, IntPtr.Size, out needed, 3)) throw new Exception("EnumProcessModulesEx failed: " + Marshal.GetLastWin32Error());
            long baseAddr = mods[0].ToInt64();
            var hdr = new byte[0x1000]; IntPtr got;
            if (!ReadProcessMemory(h, mods[0], hdr, (IntPtr)hdr.Length, out got)) throw new Exception("header read failed: " + Marshal.GetLastWin32Error());
            int pe = BitConverter.ToInt32(hdr, 0x3c);
            int opt = pe + 24;
            uint sizeOfImage = BitConverter.ToUInt32(hdr, opt + 56);
            uint stamp = BitConverter.ToUInt32(hdr, pe + 8);
            var image = new byte[sizeOfImage];
            var page = new byte[0x1000]; long unreadable = 0;
            for (long off = 0; off < sizeOfImage; off += 0x1000) {
                if (ReadProcessMemory(h, (IntPtr)(baseAddr + off), page, (IntPtr)0x1000, out got) && got.ToInt64() == 0x1000)
                    Buffer.BlockCopy(page, 0, image, (int)off, 0x1000);
                else unreadable++;
            }
            // Lay the file out as loaded: each section's raw data at its virtual address, and the
            // image base set to the runtime base so relocated pointers stay consistent.
            int sections = BitConverter.ToUInt16(image, pe + 6);
            int optSize = BitConverter.ToUInt16(image, pe + 20);
            int table = opt + optSize;
            uint sectionAlign = BitConverter.ToUInt32(image, opt + 32);
            Array.Copy(BitConverter.GetBytes(baseAddr), 0, image, opt + 24, 8);       // ImageBase
            Array.Copy(BitConverter.GetBytes(sectionAlign), 0, image, opt + 36, 4);   // FileAlignment
            var names = new System.Text.StringBuilder();
            for (int i = 0; i < sections; i++) {
                int s = table + i * 40;
                string name = System.Text.Encoding.ASCII.GetString(image, s, 8).TrimEnd('\0');
                uint vsize = BitConverter.ToUInt32(image, s + 8), va = BitConverter.ToUInt32(image, s + 12);
                uint raw = Math.Min((vsize + sectionAlign - 1) / sectionAlign * sectionAlign, sizeOfImage - va);
                Array.Copy(BitConverter.GetBytes(raw), 0, image, s + 16, 4);  // SizeOfRawData
                Array.Copy(BitConverter.GetBytes(va), 0, image, s + 20, 4);   // PointerToRawData
                names.AppendFormat("{0} va=0x{1:x} size=0x{2:x}; ", name, va, vsize);
            }
            File.WriteAllBytes(outPath, image);
            return String.Format("base=0x{0:x} timestamp=0x{1:x} sizeOfImage=0x{2:x} unreadablePages={3}\n{4}", baseAddr, stamp, sizeOfImage, unreadable, names);
        } finally { CloseHandle(h); }
    }
}
'@
$p = @(Get-Process -Name $ProcessName)
if ($p.Count -ne 1) { throw "Expected one $ProcessName process, found $($p.Count)." }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null
[ImageDump]::Dump($p[0].Id, $Out)
