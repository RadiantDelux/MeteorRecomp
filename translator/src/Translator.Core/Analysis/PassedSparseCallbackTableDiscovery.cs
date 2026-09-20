using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;

namespace Translator.Core.Analysis;

public sealed record PassedSparseCallbackTableDiscoveryResult(
    IReadOnlyList<uint> Targets,
    int TableSegmentCount,
    int CandidateTableCount);

/// <summary>
/// Finds sparse callback arrays whose exact base is passed as a call argument by translated PPC.
/// Entries may be executable pointers or null slots. To keep a passed scalar/data array from
/// becoming a function-start oracle, a table needs at least four callbacks, at least one null slot,
/// and every distinct callback must start immediately after a terminal guest transfer.
/// </summary>
public static class PassedSparseCallbackTableDiscovery
{
    private const int MinimumTargetCount = 4;
    private const int MaxTableWords = 128;
    private const uint BlrInstruction = 0x4E800020u;

    public static PassedSparseCallbackTableDiscoveryResult Discover(
        DolFile dol,
        ProgramImage image,
        IReadOnlySet<uint> passedCallArgumentConstants)
    {
        ArgumentNullException.ThrowIfNull(dol);
        ArgumentNullException.ThrowIfNull(image);
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

        var promoted = new HashSet<uint>();
        var candidates = 0;
        var admitted = 0;
        foreach (var tableAddress in passedCallArgumentConstants.OrderBy(static address => address))
        {
            if ((tableAddress & 3u) != 0)
            {
                continue;
            }

            var section = dataSections.FirstOrDefault(section => section.Range.Contains(tableAddress));
            if (section is null)
            {
                continue;
            }

            var sectionOffset = checked((int)(tableAddress - section.VirtualAddress));
            var span = section.Data.Span;
            if (sectionOffset < 0 || sectionOffset + sizeof(uint) > span.Length)
            {
                continue;
            }

            // The passed value must be the callback-array base itself, not merely a header
            // somewhere before it. That makes the usage evidence exact and keeps the scan local.
            var first = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(sectionOffset, sizeof(uint)));
            if (!LooksLikeExecutableEntry(first))
            {
                continue;
            }

            var targets = new List<uint>();
            var nullSlots = 0;
            for (var wordIndex = 0; wordIndex < MaxTableWords; wordIndex++)
            {
                var offset = sectionOffset + wordIndex * sizeof(uint);
                if (offset + sizeof(uint) > span.Length)
                {
                    break;
                }

                var value = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset, sizeof(uint)));
                if (value == 0)
                {
                    nullSlots++;
                    continue;
                }
                if (!LooksLikeExecutableEntry(value))
                {
                    break;
                }

                targets.Add(value);
            }

            var distinctTargets = targets.Distinct().ToArray();
            if (distinctTargets.Length < MinimumTargetCount || nullSlots == 0)
            {
                continue;
            }
            candidates++;

            if (!distinctTargets.All(target =>
                    ImmediatelyFollowsTerminalTransfer(image, executableRanges, target)))
            {
                continue;
            }

            var added = false;
            foreach (var target in distinctTargets)
            {
                added |= promoted.Add(target);
            }
            if (added)
            {
                admitted++;
            }
        }

        return new PassedSparseCallbackTableDiscoveryResult(
            promoted.OrderBy(static address => address).ToArray(),
            admitted,
            candidates);
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
