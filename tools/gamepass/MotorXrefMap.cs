using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

public static class XrefMap {
    public class Pe { public byte[] Img; public int TextLo, TextHi; public List<Tuple<string,int,int>> Sections = new List<Tuple<string,int,int>>(); }

    // Lays a PE file out by RVA (a dump written with raw offset = VA lays out the same way).
    public static Pe Load(string path) {
        var f = File.ReadAllBytes(path);
        int pe = BitConverter.ToInt32(f, 0x3c), opt = pe + 24;
        int n = BitConverter.ToUInt16(f, pe + 6), optSize = BitConverter.ToUInt16(f, pe + 20);
        uint size = BitConverter.ToUInt32(f, opt + 56), hdr = BitConverter.ToUInt32(f, opt + 60);
        var r = new Pe { Img = new byte[size] };
        Buffer.BlockCopy(f, 0, r.Img, 0, (int)Math.Min(hdr, (uint)f.Length));
        for (int i = 0; i < n; i++) {
            int s = opt + optSize + i * 40;
            string name = System.Text.Encoding.ASCII.GetString(f, s, 8).TrimEnd('\0');
            int vsize = BitConverter.ToInt32(f, s + 8), va = BitConverter.ToInt32(f, s + 12);
            int rsize = BitConverter.ToInt32(f, s + 16), raw = BitConverter.ToInt32(f, s + 20);
            int len = Math.Min(Math.Min(rsize, vsize), (int)size - va); len = Math.Min(len, f.Length - raw);
            if (len > 0) Buffer.BlockCopy(f, raw, r.Img, va, len);
            r.Sections.Add(Tuple.Create(name, va, vsize));
            if (name == ".text") { r.TextLo = va; r.TextHi = va + vsize; }
        }
        return r;
    }

    // Operand bytes that move between builds: rel32 calls/jumps/jcc and RIP-relative displacements.
    public static bool[] Mask(byte[] img, int start, int len) {
        var m = new bool[len];
        for (int i = 0; i < len; i++) {
            byte b = img[start + i];
            if (b == 0xE8 || b == 0xE9) for (int k = 1; k <= 4 && i + k < len; k++) m[i + k] = true;
            if (b == 0x0F && i + 1 < len && (img[start + i + 1] & 0xF0) == 0x80) for (int k = 2; k <= 5 && i + k < len; k++) m[i + k] = true;
            if (i > 0 && (b & 0xC7) == 0x05) for (int k = 1; k <= 4 && i + k < len; k++) m[i + k] = true;
        }
        return m;
    }

    public static List<int> Find(byte[] hay, int lo, int hi, byte[] src, int start, int len, bool[] mask, int max) {
        var hits = new List<int>();
        int a = 0; while (a < len && mask[a]) a++;
        if (a == len) return hits;
        byte first = src[start + a];
        lo = Math.Max(lo, 0); hi = Math.Min(hi, hay.Length - len);
        for (int p = lo + a; p < hi + a; p++) {
            p = Array.IndexOf(hay, first, p, hi + a - p);
            if (p < 0) break;
            int s = p - a; bool ok = true;
            for (int i = 0; i < len; i++) if (!mask[i] && hay[s + i] != src[start + i]) { ok = false; break; }
            if (ok) { hits.Add(s); if (hits.Count >= max) break; }
        }
        return hits;
    }

    // Steam->GamePass code shift predicted by the nearest anchor at or below the Steam RVA.
    static long[] ancS, ancG;
    public static void SetAnchors(long[] steam, long[] gp) {
        var idx = Enumerable.Range(0, steam.Length).OrderBy(i => steam[i]).ToArray();
        ancS = idx.Select(i => steam[i]).ToArray(); ancG = idx.Select(i => gp[i]).ToArray();
    }
    public static long ShiftAt(long s) { return Shift(s); }
    static long Shift(long s) {
        int i = Array.BinarySearch(ancS, s); if (i < 0) i = ~i;
        int best = -1; long d = long.MaxValue;
        foreach (int j in new[] { i - 1, i }) if (j >= 0 && j < ancS.Length && Math.Abs(ancS[j] - s) < d) { d = Math.Abs(ancS[j] - s); best = j; }
        return ancG[best] - ancS[best];
    }

    // Locates a Steam code position in Game Pass by its surrounding bytes. Returns -1 when not unique.
    public static long Locate(Pe steam, Pe gp, long s, int before, int after, out int hits) {
        int start = (int)s - before, len = before + after;
        var mask = Mask(steam.Img, start, len);
        long c = s + Shift(s);
        foreach (int win in new[] { 0x10000, 0x80000 }) {
            var h = Find(gp.Img, (int)Math.Max(gp.TextLo, c - win), (int)Math.Min(gp.TextHi, c + win), steam.Img, start, len, mask, 3);
            hits = h.Count;
            if (h.Count == 1) return h[0] + before;
            if (h.Count > 1) return -1;
        }
        hits = 0; return -1;
    }

    // All Game Pass positions matching the Steam context near the predicted spot (up to max).
    public static List<int> LocateAll(Pe steam, Pe gp, long s, int before, int after, int win, int max) {
        int start = (int)s - before, len = before + after;
        var mask = Mask(steam.Img, start, len);
        long c = s + Shift(s);
        var h = Find(gp.Img, (int)Math.Max(gp.TextLo, c - win), (int)Math.Min(gp.TextHi, c + win), steam.Img, start, len, mask, max);
        return h.Select(x => x + before).ToList();
    }

    // Every RIP-relative reference in Steam .text whose target is one of the given RVAs.
    // Returns (site of disp32, trailing immediate size, target).
    public static List<long[]> Xrefs(Pe steam, HashSet<long> targets) {
        var r = new List<long[]>();
        byte[] img = steam.Img;
        for (int p = steam.TextLo + 1; p < steam.TextHi - 4; p++) {
            if ((img[p - 1] & 0xC7) != 0x05) continue;
            int disp = BitConverter.ToInt32(img, p);
            foreach (int extra in new[] { 0, 1, 2, 4 }) {
                long t = p + 4 + extra + (long)disp;
                if (targets.Contains(t)) r.Add(new long[] { p, extra, t });
            }
        }
        return r;
    }

    public static int ReadInt(Pe pe, long at) { return BitConverter.ToInt32(pe.Img, (int)at); }
}
