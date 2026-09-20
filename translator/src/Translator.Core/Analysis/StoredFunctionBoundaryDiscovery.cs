using System.Buffers.Binary;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;

namespace Translator.Core.Analysis;

/// <summary>
/// A directly materialized/stored code pointer can identify an otherwise unused
/// callback, including a one-instruction identity callback. Require an existing
/// decoded exclusive end, a raw blr predecessor, executable bytes and no covered
/// instruction at the target. Merely finding a code-looking integer is not enough.
/// </summary>
public static class StoredFunctionBoundaryDiscovery
{
    public static IReadOnlyList<uint> Discover(DolFile dol, ProgramImage image,
        IReadOnlySet<uint> storedPointers, IReadOnlySet<uint> decodedEnds,
        IReadOnlySet<uint> coveredInstructions)
    {
        var ranges = dol.Sections.Where(s => s.IsExecutable && s.Size != 0).Select(s => s.Range).ToArray();
        var result = new List<uint>();
        foreach (var target in storedPointers.OrderBy(x => x))
        {
            if (target < 4 || (target & 3) != 0 ||
                coveredInstructions.Contains(target) ||
                !ranges.Any(r => r.Contains(target - 4) && r.Contains(target)) ||
                !image.Contains(target - 4, 8)) continue;
            var offset = image.GetOffset(target - 4, 8);
            if (BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(offset, 4)) != 0x4e800020u) continue;
            var word = BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(offset + 4, 4));
            if (word == 0 || word == uint.MaxValue || PpcDecoder.Decode(target, word).Mnemonic.StartsWith("unk", StringComparison.OrdinalIgnoreCase)) continue;
            if (!decodedEnds.Contains(target))
            {
                // Empty callbacks can be adjacent. A decoder may fold a shared
                // conditional return into its IR and stop before a run of blr
                // words. Only a stored, one-instruction blr callback may bridge
                // that terminal-only gap; arbitrary code or covered flow cannot.
                if (word != 0x4e800020u) continue;
                var anchored = false;
                for (var at = target - 4; target - at <= 32 && at >= 4; at -= 4)
                {
                    if (coveredInstructions.Contains(at) || !image.Contains(at, 4) ||
                        !ranges.Any(r => r.Contains(at)) ||
                        BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(image.GetOffset(at, 4), 4)) != 0x4e800020u) break;
                    if (decodedEnds.Contains(at)) { anchored = true; break; }
                }
                if (!anchored) continue;
            }
            result.Add(target);
        }
        return result;
    }
}
