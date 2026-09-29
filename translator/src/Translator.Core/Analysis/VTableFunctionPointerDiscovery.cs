using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;

namespace Translator.Core.Analysis;

public sealed record VTableFunctionPointerDiscoveryResult(
    IReadOnlyList<uint> Targets,
    int TableSegmentCount,
    int PointerEntryCount);

/// <summary>
/// Conservatively discovers CodeWarrior-style virtual-table method pointers in initialized DOL data.
///
/// A raw "data word points into .text" scan is not safe enough because switch/jump tables use the
/// same representation. The tables seen in retail CodeWarrior Wii binaries carry an RTTI/data
/// pointer and a small signed this-adjustment immediately before a run of method pointers. Requiring
/// that preamble plus at least two consecutive executable entries keeps this useful as a function
/// boundary oracle without turning arbitrary switch destinations into functions.
/// </summary>
public static class VTableFunctionPointerDiscovery
{
    private const int MaxThisAdjustment = 0x1000;
    private const int MinimumMethodRunLength = 2;

    public static VTableFunctionPointerDiscoveryResult Discover(DolFile dol, ProgramImage image)
    {
        ArgumentNullException.ThrowIfNull(dol);
        ArgumentNullException.ThrowIfNull(image);

        var executableRanges = dol.Sections
            .Where(static section => section.IsExecutable && section.Size != 0)
            .Select(static section => section.Range)
            .ToArray();
        var initializedDataRanges = dol.Sections
            .Where(static section => section.Kind == SectionKind.Data && section.Size != 0)
            .Select(static section => section.Range)
            .ToArray();

        bool IsExecutable(uint address) =>
            (address & 3u) == 0 && executableRanges.Any(range => range.Contains(address));
        bool IsInitializedData(uint address) =>
            initializedDataRanges.Any(range => range.Contains(address));

        bool LooksLikeFunctionEntry(uint address)
        {
            if (!IsExecutable(address) || !image.Contains(address, sizeof(uint)))
            {
                return false;
            }

            var offset = image.GetOffset(address, sizeof(uint));
            var word = BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(offset, sizeof(uint)));
            if (word is 0 or 0xFFFFFFFFu)
            {
                return false;
            }

            var instruction = PpcDecoder.Decode(address, word);
            return !instruction.Mnemonic.StartsWith("unk", StringComparison.OrdinalIgnoreCase);
        }

        static bool LooksLikeThisAdjustment(uint raw)
        {
            var signed = unchecked((int)raw);
            return (signed & 3) == 0 && signed >= -MaxThisAdjustment && signed <= MaxThisAdjustment;
        }

        var targets = new HashSet<uint>();
        var tableSegments = 0;
        var pointerEntries = 0;

        foreach (var section in dol.Sections.Where(static section =>
                     section.Kind == SectionKind.Data && section.HasData && section.Size >= 16))
        {
            var span = section.Data.Span;
            for (var cursor = 0; cursor + 16 <= span.Length; cursor += 4)
            {
                var rttiPointer = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(cursor, 4));
                var thisAdjustment = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(cursor + 4, 4));
                if (!IsInitializedData(rttiPointer) || !LooksLikeThisAdjustment(thisAdjustment))
                {
                    continue;
                }

                var methodCursor = cursor + 8;
                var run = new List<uint>();
                while (methodCursor + 4 <= span.Length)
                {
                    var candidate = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(methodCursor, 4));
                    if (!LooksLikeFunctionEntry(candidate))
                    {
                        break;
                    }

                    run.Add(candidate);
                    methodCursor += 4;
                }

                if (run.Count < MinimumMethodRunLength)
                {
                    continue;
                }

                tableSegments++;
                pointerEntries += run.Count;
                foreach (var target in run)
                {
                    targets.Add(target);
                }

                // Do not rediscover overlapping suffixes of the same method run.
                cursor = methodCursor - 4;
            }
        }

        return new VTableFunctionPointerDiscoveryResult(
            targets.OrderBy(static address => address).ToArray(),
            tableSegments,
            pointerEntries);
    }
}
