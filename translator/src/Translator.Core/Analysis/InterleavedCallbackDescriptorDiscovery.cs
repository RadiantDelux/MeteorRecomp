using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;

namespace Translator.Core.Analysis;

public sealed record InterleavedCallbackDescriptorDiscoveryResult(
    IReadOnlyList<uint> Targets,
    int CandidateDescriptorCount,
    int AdmittedDescriptorCount);

/// <summary>
/// Finds short callback descriptors whose code-pointer fields are interleaved with scalar/null
/// fields. The descriptor shape is not used as an oracle by itself: scanning starts only at an
/// exact data address that translated PPC has either stored into an object field or passed as a
/// call argument. A candidate must contain several executable pointers, contain at least one
/// non-code field between its first and last callback slot, and every callback must already be a
/// proven function start or begin immediately after a terminal guest transfer.
///
/// This complements dense vtable and fixed-stride descriptor discovery. In particular it handles
/// small interface/state records where callback slots are separated by flags, nulls, IDs, or other
/// scalar words, including one-instruction blr/tail-branch thunks, without treating dense switch
/// tables as function-boundary evidence.
/// </summary>
public static class InterleavedCallbackDescriptorDiscovery
{
    private const int MinimumTargetCount = 3;
    private const int MaxDescriptorWords = 12;
    private const int MaxTrailingNonCodeWords = 2;
    private const uint BlrInstruction = 0x4E800020u;

    public static InterleavedCallbackDescriptorDiscoveryResult Discover(
        DolFile dol,
        ProgramImage image,
        IReadOnlySet<uint> provenFunctionStarts,
        IReadOnlySet<uint> storedPointerValues,
        IReadOnlySet<uint> passedCallArgumentConstants)
    {
        ArgumentNullException.ThrowIfNull(dol);
        ArgumentNullException.ThrowIfNull(image);
        ArgumentNullException.ThrowIfNull(provenFunctionStarts);
        ArgumentNullException.ThrowIfNull(storedPointerValues);
        ArgumentNullException.ThrowIfNull(passedCallArgumentConstants);

        var executableRanges = dol.Sections
            .Where(static section => section.IsExecutable && section.Size != 0)
            .Select(static section => section.Range)
            .ToArray();
        var dataSections = dol.Sections
            .Where(static section => section.Kind == SectionKind.Data && section.HasData && section.Size >= sizeof(uint))
            .ToArray();

        bool LooksLikeExecutableEntry(uint address)
        {
            if ((address & 3u) != 0 ||
                !executableRanges.Any(range => range.Contains(address)) ||
                !image.Contains(address, sizeof(uint)))
            {
                return false;
            }

            var offset = image.GetOffset(address, sizeof(uint));
            var word = BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(offset, sizeof(uint)));
            return word is not (0 or 0xFFFFFFFFu) &&
                   !PpcDecoder.Decode(address, word).Mnemonic.StartsWith("unk", StringComparison.OrdinalIgnoreCase);
        }

        var evidenceBases = storedPointerValues
            .Concat(passedCallArgumentConstants)
            .Distinct()
            .OrderBy(static address => address);
        var promoted = new HashSet<uint>();
        var candidates = 0;
        var admitted = 0;

        foreach (var descriptorAddress in evidenceBases)
        {
            if ((descriptorAddress & 3u) != 0)
            {
                continue;
            }

            var section = dataSections.FirstOrDefault(section => section.Range.Contains(descriptorAddress));
            if (section is null)
            {
                continue;
            }

            var startOffset = checked((int)(descriptorAddress - section.VirtualAddress));
            var span = section.Data.Span;
            if (startOffset < 0 || startOffset + sizeof(uint) > span.Length)
            {
                continue;
            }

            var targets = new List<uint>();
            var targetWordIndices = new List<int>();
            var trailingNonCodeWords = 0;
            for (var wordIndex = 0; wordIndex < MaxDescriptorWords; wordIndex++)
            {
                var offset = startOffset + wordIndex * sizeof(uint);
                if (offset + sizeof(uint) > span.Length)
                {
                    break;
                }

                var value = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset, sizeof(uint)));
                if (LooksLikeExecutableEntry(value))
                {
                    targets.Add(value);
                    targetWordIndices.Add(wordIndex);
                    trailingNonCodeWords = 0;
                    continue;
                }

                trailingNonCodeWords++;
                if (targets.Count >= MinimumTargetCount && trailingNonCodeWords >= MaxTrailingNonCodeWords)
                {
                    break;
                }
            }

            var distinctTargets = targets.Distinct().ToArray();
            if (distinctTargets.Length < MinimumTargetCount || targetWordIndices.Count < MinimumTargetCount)
            {
                continue;
            }

            var firstTargetWord = targetWordIndices[0];
            var lastTargetWord = targetWordIndices[^1];
            var hasInterleavedNonCodeField = targetWordIndices.Count != lastTargetWord - firstTargetWord + 1;
            if (!hasInterleavedNonCodeField)
            {
                continue;
            }

            candidates++;
            if (!distinctTargets.All(target =>
                    provenFunctionStarts.Contains(target) ||
                    ImmediatelyFollowsTerminalTransfer(image, executableRanges, target)))
            {
                continue;
            }

            var added = false;
            foreach (var target in distinctTargets)
            {
                if (provenFunctionStarts.Contains(target) ||
                    !ImmediatelyFollowsTerminalTransfer(image, executableRanges, target))
                {
                    continue;
                }

                added |= promoted.Add(target);
            }

            if (added)
            {
                admitted++;
            }
        }

        return new InterleavedCallbackDescriptorDiscoveryResult(
            promoted.OrderBy(static address => address).ToArray(),
            candidates,
            admitted);
    }

    private static bool ImmediatelyFollowsTerminalTransfer(
        ProgramImage image,
        IReadOnlyList<AddressRange> executableRanges,
        uint address)
    {
        if (address < sizeof(uint))
        {
            return false;
        }

        var previousAddress = address - sizeof(uint);
        if (!executableRanges.Any(range => range.Contains(previousAddress)) ||
            !image.Contains(previousAddress, sizeof(uint)))
        {
            return false;
        }

        var offset = image.GetOffset(previousAddress, sizeof(uint));
        var previousWord = BinaryPrimitives.ReadUInt32BigEndian(
            image.Memory.AsSpan(offset, sizeof(uint)));
        if (previousWord == BlrInstruction)
        {
            return true;
        }

        return PpcDecoder.Decode(previousAddress, previousWord).IsUnconditionalBranch;
    }
}
