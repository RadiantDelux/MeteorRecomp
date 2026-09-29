using System.Text;
using Translator.Core.Parsing.Rso;

namespace Translator.Core.Translation;

public sealed record RsoDynamicTemplateSignature(byte[] Expected, byte[] CompareMask);

/// <summary>
/// Emits the runtime-owned registrar for an AOT dynamic RSO template. The
/// generated resolver compares only bytes that are invariant across legal RSO
/// runtime link bases; every linker-mutated field is masked explicitly from
/// <see cref="RsoFile"/> relocation/header metadata.
/// </summary>
public static class RsoDynamicTemplateSourceWriter
{
    public static RsoDynamicTemplateSignature BuildInvariantSignature(
        RsoFile file,
        RsoLinkedImage linked,
        uint? runtimeImageSize = null)
    {
        ArgumentNullException.ThrowIfNull(file);
        ArgumentNullException.ThrowIfNull(linked);
        if (file.RawData.Length != linked.Image.Length)
            throw new InvalidDataException("RSO linked image size differs from pre-link image size");

        var retainedSize = runtimeImageSize ?? checked((uint)linked.Image.Length);
        if (retainedSize == 0 || retainedSize > linked.Image.Length)
            throw new ArgumentOutOfRangeException(nameof(runtimeImageSize));
        var expected = linked.Image.AsSpan(0, checked((int)retainedSize)).ToArray();
        // Bit masks, not byte-presence flags. Branch relocations replace only
        // the displacement: opcode, AA/LK and conditional BO/BI bits remain
        // part of the translated instruction's identity.
        var mask = Enumerable.Repeat(byte.MaxValue, expected.Length).ToArray();
        void Ignore(uint offset, int length, string label)
        {
            if ((ulong)offset + (uint)length > (ulong)mask.Length)
                throw new InvalidDataException($"RSO dynamic signature {label} is outside the image");
            Array.Clear(mask, checked((int)offset), length);
        }

        // Runtime-mutated RSO header fields. Besides rebased pointers, RSOLink
        // links modules into a list and consumes/clears selected relocation and
        // import table sizes after processing them. None of these words are
        // stable module identity bytes.
        foreach (var offset in new uint[]
        {
            0x00, 0x04,
            0x0C, 0x10, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C,
            0x40, 0x48, 0x4C, 0x50, 0x54,
        })
            Ignore(offset, 4, $"header field +0x{offset:X}");

        // Each linked section record's first word becomes a runtime address.
        for (var index = 0; index < file.Sections.Count; ++index)
            Ignore(checked(file.Header.SectionTableOffset + (uint)index * RsoFile.SectionEntrySize), 4, $"section {index} address");

        void IgnoreRelocationTablePatchAddresses(uint tableOffset, int count, string label)
        {
            if (count == 0 || tableOffset >= retainedSize)
                return;
            var tableEnd = (ulong)tableOffset + (ulong)(uint)count * RsoFile.RelocationEntrySize;
            if (tableEnd > retainedSize)
                throw new InvalidDataException($"RSO dynamic signature {label} relocation table straddles the retained runtime image");
            for (var index = 0; index < count; ++index)
                Ignore(checked(tableOffset + (uint)index * RsoFile.RelocationEntrySize), 4, $"{label} relocation {index} patch address");
        }
        IgnoreRelocationTablePatchAddresses(file.Header.InternalRelocationTableOffset, file.InternalRelocations.Count, "internal");
        IgnoreRelocationTablePatchAddresses(file.Header.ExternalRelocationTableOffset, file.ExternalRelocations.Count, "external");

        // Mask every relocation site described by the RSO, not only the
        // relocations that the canonical analysis link could resolve. Retail
        // may resolve imports from modules that are absent from the selectable
        // export set used by the offline translator, and those legal runtime
        // patches must not make the invariant signature reject the module.
        foreach (var relocation in file.InternalRelocations.Concat(file.ExternalRelocations))
        {
            var width = RelocationPatchWidth(relocation.Type);
            if (width == 0)
                continue;
            var offset = relocation.Offset;
            if (offset >= retainedSize)
                continue;
            if ((ulong)offset + (uint)width > retainedSize)
                throw new InvalidDataException($"RSO dynamic signature relocation patch +0x{offset:X8} is outside the image");
            var replacedBits = RelocationPatchBits(relocation.Type);
            for (var index = 0; index < width; ++index)
            {
                var shift = (width - index - 1) * 8;
                mask[checked((int)offset + index)] &= (byte)~(replacedBits >> shift);
            }
        }

        return new RsoDynamicTemplateSignature(expected, mask);
    }

    public static string WriteRegistrarSource(
        RsoFile file,
        RsoDynamicTemplateCompilation compilation,
        string? expectedBinaryPath = null,
        string? compareMaskBinaryPath = null)
    {
        ArgumentNullException.ThrowIfNull(file);
        ArgumentNullException.ThrowIfNull(compilation);
        // Discovery happens while the retail linker still owns the complete RSO
        // image, including relocation/import metadata that may be discarded by
        // RSOLinkListFixed afterwards. Keep the discovery signature complete;
        // TemplateContext.ImageSize is the post-link retained range used only
        // for steady-state dispatch validation.
        var signature = BuildInvariantSignature(file, compilation.LinkedImage);
        var id = SanitizeIdentifier(compilation.TemplateContext.TemplateId);
        var bssSection = compilation.LinkedImage.Sections.SingleOrDefault(static section => section.IsBss);
        if (compilation.TemplateContext.BssSize != 0 && bssSection is null)
            throw new InvalidDataException("Dynamic RSO has BSS bytes but no linked BSS section");
        var bssEntryOffset = bssSection is null
            ? 0u
            : checked(file.Header.SectionTableOffset + (uint)bssSection.Index * RsoFile.SectionEntrySize);

        var publicationOffsets = BuildPublicationOffsets(file, compilation);

        var sb = new StringBuilder(32_768);
        sb.AppendLine("#include <cstddef>");
        sb.AppendLine("#include <cstdint>");
        sb.AppendLine("#include <cstdlib>");
        sb.AppendLine("#include \"dynamic_module.h\"");
        sb.AppendLine("#include \"dynamic_module_signature.h\"");
        sb.AppendLine("#include \"memory.h\"");
        sb.AppendLine();
        foreach (var function in compilation.Functions)
        {
            var facts = function.Translation.Emission?.DynamicDispatch
                ?? throw new InvalidDataException($"Dynamic function 0x{function.Address:X8} has no structured dispatch facts");
            sb.AppendLine($"extern \"C\" void {facts.Symbol}(CpuContext* ctx);");
        }
        sb.AppendLine();
        sb.AppendLine("namespace {");
        if ((expectedBinaryPath is null) != (compareMaskBinaryPath is null))
            throw new ArgumentException("Dynamic RSO signature binary paths must either both be supplied or both be omitted");
        if (expectedBinaryPath is not null)
        {
            AppendIncbinArray(sb, $"kExpected_{id}", expectedBinaryPath);
            AppendIncbinArray(sb, $"kCompareMask_{id}", compareMaskBinaryPath!);
        }
        else
        {
            AppendByteArray(sb, $"kExpected_{id}", signature.Expected);
            AppendByteArray(sb, $"kCompareMask_{id}", signature.CompareMask);
        }
        AppendUInt32Array(sb, $"kPublicationOffsets_{id}", publicationOffsets);
        AppendByteValueArray(
            sb,
            $"kResolvedImports_{id}",
            compilation.LinkedImage.Imports.Select(static import => import.IsResolved ? (byte)1 : (byte)0).ToArray());
        sb.AppendLine();
        sb.AppendLine($"bool QuickMatch_{id}(uint32_t base) {{");
        sb.AppendLine("    if (!Memory::Contains(base, 0x14u)) return false;");
        sb.AppendLine("    try {");
        sb.AppendLine($"        if (Memory::Read32(base + 0x0Cu) != base + 0x{file.Header.SectionTableOffset:X8}u) return false;");
        sb.AppendLine($"        if (Memory::Read32(base + 0x10u) != base + 0x{file.Header.ModuleNameOffset:X8}u) return false;");
        sb.AppendLine("        return true;");
        sb.AppendLine("    } catch (const Memory::AccessViolation&) {");
        sb.AppendLine("        return false;");
        sb.AppendLine("    }");
        sb.AppendLine("}");
        sb.AppendLine();
        var fullImageSize = checked((uint)compilation.LinkedImage.Image.Length);
        var retainedImageSize = compilation.TemplateContext.ImageSize;
        sb.AppendLine($"bool Match_{id}(uint32_t base, DynamicModule::InstanceLayout* layout) {{");
        sb.AppendLine($"    constexpr uint32_t kFullImageSize = 0x{fullImageSize:X8}u;");
        sb.AppendLine("    if (!Memory::Contains(base, kFullImageSize)) return false;");
        sb.AppendLine("    try {");
        sb.AppendLine("        MemoryInline::ResolveDeferredReads(base, kFullImageSize);");
        sb.AppendLine("        const auto* image = Memory::GetPointer(base, kFullImageSize);");
        sb.AppendLine($"        if (!DynamicModule::MatchMaskedImage(image, kExpected_{id}, kCompareMask_{id}, kFullImageSize)) return false;");
        sb.AppendLine("        uint32_t bss = 0;");
        if (bssSection is not null)
        {
            sb.AppendLine($"        bss = DynamicModule::ReadImage32(image, 0x{bssEntryOffset:X8}u);");
            sb.AppendLine($"        if (bss == 0 || !Memory::Contains(bss, 0x{compilation.TemplateContext.BssSize:X8}u)) return false;");
        }
        AppendLinkedLayoutValidation(sb, file, compilation, fullImageSize);
        sb.AppendLine("        if (layout != nullptr) { layout->imageBase = base; layout->bssBase = bss; }");
        sb.AppendLine("        return true;");
        sb.AppendLine("    } catch (const Memory::AccessViolation&) {");
        sb.AppendLine("        return false;");
        sb.AppendLine("    }");
        sb.AppendLine("}");
        sb.AppendLine();
        sb.AppendLine($"bool ValidateDispatch_{id}(uint32_t base, DynamicModule::InstanceLayout* layout) {{");
        sb.AppendLine($"    constexpr uint32_t kRetainedImageSize = 0x{retainedImageSize:X8}u;");
        sb.AppendLine("    if (!Memory::Contains(base, kRetainedImageSize)) return false;");
        sb.AppendLine("    try {");
        sb.AppendLine($"        if (!QuickMatch_{id}(base)) return false;");
        sb.AppendLine("        uint32_t bss = 0;");
        if (bssSection is not null)
        {
            sb.AppendLine($"        bss = Memory::Read32(base + 0x{bssEntryOffset:X8}u);");
            sb.AppendLine($"        if (bss == 0 || !Memory::Contains(bss, 0x{compilation.TemplateContext.BssSize:X8}u)) return false;");
        }
        sb.AppendLine("        if (layout != nullptr) { layout->imageBase = base; layout->bssBase = bss; }");
        sb.AppendLine("        return true;");
        sb.AppendLine("    } catch (const Memory::AccessViolation&) {");
        sb.AppendLine("        return false;");
        sb.AppendLine("    }");
        sb.AppendLine("}");
        sb.AppendLine();
        sb.AppendLine($"bool ValidateRetained_{id}(uint32_t base, DynamicModule::InstanceLayout* layout) {{");
        sb.AppendLine($"    constexpr uint32_t kRetainedImageSize = 0x{retainedImageSize:X8}u;");
        sb.AppendLine("    if (!Memory::Contains(base, kRetainedImageSize)) return false;");
        sb.AppendLine("    try {");
        sb.AppendLine("        MemoryInline::ResolveDeferredReads(base, kRetainedImageSize);");
        sb.AppendLine("        const auto* image = Memory::GetPointer(base, kRetainedImageSize);");
        sb.AppendLine($"        if (!DynamicModule::MatchMaskedImage(image, kExpected_{id}, kCompareMask_{id}, kRetainedImageSize)) return false;");
        sb.AppendLine("        uint32_t bss = 0;");
        if (bssSection is not null)
        {
            sb.AppendLine($"        bss = DynamicModule::ReadImage32(image, 0x{bssEntryOffset:X8}u);");
            sb.AppendLine($"        if (bss == 0 || !Memory::Contains(bss, 0x{compilation.TemplateContext.BssSize:X8}u)) return false;");
        }
        AppendLinkedLayoutValidation(sb, file, compilation, retainedImageSize);
        sb.AppendLine("        if (layout != nullptr) { layout->imageBase = base; layout->bssBase = bss; }");
        sb.AppendLine("        return true;");
        sb.AppendLine("    } catch (const Memory::AccessViolation&) {");
        sb.AppendLine("        return false;");
        sb.AppendLine("    }");
        sb.AppendLine("}");
        sb.AppendLine();
        sb.AppendLine($"bool Resolve_{id}(uint32_t probe, DynamicModule::InstanceLayout* layout) {{");
        sb.AppendLine("    bool found = false;");
        sb.AppendLine("    DynamicModule::InstanceLayout resolved{};");
        sb.AppendLine($"    for (const uint32_t offset : kPublicationOffsets_{id}) {{");
        sb.AppendLine("        if (probe < offset) continue;");
        sb.AppendLine("        const uint32_t base = probe - offset;");
        sb.AppendLine($"        if (!QuickMatch_{id}(base)) continue;");
        sb.AppendLine($"        if (DynamicModule::InstanceLayout candidate{{}}; Match_{id}(base, &candidate)) {{");
        sb.AppendLine("            if (found && resolved.imageBase != candidate.imageBase) return false;");
        sb.AppendLine("            resolved = candidate; found = true;");
        sb.AppendLine("        }");
        sb.AppendLine("    }");
        sb.AppendLine("    if (!found) return false;");
        sb.AppendLine("    if (layout != nullptr) *layout = resolved;");
        sb.AppendLine("    return true;");
        sb.AppendLine("}");
        sb.AppendLine();
        sb.AppendLine($"const DynamicModule::FunctionRecord kFunctions_{id}[] = {{");
        foreach (var function in compilation.Functions)
        {
            var facts = function.Translation.Emission!.DynamicDispatch!;
            sb.AppendLine(
                $"    {{0x{function.Offset:X8}u, &{facts.Symbol}, " +
                $"{(facts.PreserveNonvolatileGprs ? "true" : "false")}, 0x{facts.NonvolatileFprWriteMask:X8}u}},");
        }
        sb.AppendLine("};");
        sb.AppendLine();
        sb.AppendLine($"const DynamicModule::TemplateRecord kTemplate_{id} = {{");
        sb.Append("    {");
        for (var index = 0; index < compilation.LinkedImage.PreLinkSha256.Length; ++index)
        {
            if (index != 0) sb.Append(',');
            sb.Append($"0x{compilation.LinkedImage.PreLinkSha256[index]:X2}");
        }
        sb.AppendLine("},");
        sb.AppendLine($"    0x{compilation.TemplateContext.CanonicalImageBase:X8}u,");
        sb.AppendLine($"    0x{compilation.TemplateContext.ImageSize:X8}u,");
        sb.AppendLine($"    0x{compilation.TemplateContext.CanonicalBssBase:X8}u,");
        sb.AppendLine($"    0x{compilation.TemplateContext.BssSize:X8}u,");
        sb.AppendLine($"    kFunctions_{id},");
        sb.AppendLine($"    sizeof(kFunctions_{id}) / sizeof(kFunctions_{id}[0]),");
        sb.AppendLine($"    &Resolve_{id},");
        sb.AppendLine($"    \"{EscapeString(compilation.TemplateContext.TemplateId)}\",");
        sb.AppendLine($"    &ValidateRetained_{id},");
        sb.AppendLine($"    &ValidateDispatch_{id}");
        sb.AppendLine("};");
        sb.AppendLine();
        sb.AppendLine($"struct Registrar_{id} {{");
        sb.AppendLine($"    Registrar_{id}() {{ if (!DynamicModule::RegisterTemplate(&kTemplate_{id})) std::abort(); }}");
        sb.AppendLine("};");
        sb.AppendLine($"const Registrar_{id} kRegistrar_{id}{{}};");
        sb.AppendLine("} // namespace");
        return sb.ToString();
    }

    internal static IReadOnlyList<uint> BuildPublicationOffsets(
        RsoFile file,
        RsoDynamicTemplateCompilation compilation)
    {
        ArgumentNullException.ThrowIfNull(file);
        ArgumentNullException.ThrowIfNull(compilation);

        var offsets = new SortedSet<uint> { 0u };
        foreach (var sectionIndex in compilation.ExecutableSectionIndices)
        {
            var section = file.Sections[sectionIndex];
            offsets.Add(section.FileOffset & ~31u);
        }

        // Retail LocateObject relocates the complete internal table first, then
        // walks the complete external table in order. RSORelocate flushes and
        // invalidates the cache line for every patch. Therefore the final entry
        // in the external table (or the final internal entry when there is no
        // external table) is a deterministic completion publication: by that
        // icbi all initial relocation payloads have been written. Watching every
        // relocation line is semantically unnecessary and turns a large RSO into
        // O(relocations * unique-lines) resolver work while it is being linked.
        // Keep the image/executable probes as fallbacks for already-linked image
        // publication, and one terminal linker probe for normal retail loading.
        var terminalRelocation = file.ExternalRelocations.Count != 0
            ? file.ExternalRelocations[^1]
            : file.InternalRelocations.Count != 0
                ? file.InternalRelocations[^1]
                : null;
        if (terminalRelocation is not null)
        {
            var offset = terminalRelocation.Offset;
            if (offset >= compilation.TemplateContext.ImageSize)
                throw new InvalidDataException("Dynamic RSO relocation patch lies outside the linked image");
            offsets.Add(offset & ~31u);
        }

        return offsets.ToArray();
    }

    private static void AppendLinkedLayoutValidation(
        StringBuilder sb,
        RsoFile file,
        RsoDynamicTemplateCompilation compilation,
        uint validationImageSize)
    {
        static string RebasedFileOffset(uint offset) => offset == 0 ? "0u" : $"base + 0x{offset:X8}u";

        void HeaderPointer(uint fieldOffset, uint rawOffset)
        {
            sb.AppendLine(
                $"        if (DynamicModule::ReadImage32(image, 0x{fieldOffset:X8}u) != {RebasedFileOffset(rawOffset)}) return false;");
        }

        HeaderPointer(0x0Cu, file.Header.SectionTableOffset);
        HeaderPointer(0x10u, file.Header.ModuleNameOffset);
        HeaderPointer(0x30u, file.Header.InternalRelocationTableOffset);
        HeaderPointer(0x38u, file.Header.ExternalRelocationTableOffset);
        HeaderPointer(0x40u, file.Header.ExportTableOffset);
        HeaderPointer(0x48u, file.Header.ExportNameTableOffset);
        HeaderPointer(0x4Cu, file.Header.ImportTableOffset);
        HeaderPointer(0x54u, file.Header.ImportNameTableOffset);

        void LifecyclePointer(uint fieldOffset, uint? canonicalAddress)
        {
            var expected = canonicalAddress is null
                ? "0u"
                : RuntimeAddressExpression(compilation, canonicalAddress.Value);
            sb.AppendLine($"        if (DynamicModule::ReadImage32(image, 0x{fieldOffset:X8}u) != {expected}) return false;");
        }

        LifecyclePointer(0x24u, compilation.LinkedImage.PrologAddress);
        LifecyclePointer(0x28u, compilation.LinkedImage.EpilogAddress);
        LifecyclePointer(0x2Cu, compilation.LinkedImage.UnresolvedAddress);

        foreach (var section in compilation.LinkedImage.Sections)
        {
            var entryOffset = checked(file.Header.SectionTableOffset + (uint)section.Index * RsoFile.SectionEntrySize);
            var expected = RuntimeAddressExpression(compilation, section.Address);
            sb.AppendLine($"        if (DynamicModule::ReadImage32(image, 0x{entryOffset:X8}u) != {expected}) return false;");
        }

        // Relocation tables can contain hundreds of thousands of records. Keep
        // the registrar compact by validating the live records with generic
        // loops instead of emitting one C++ statement per relocation. The
        // canonical linked image already contains the expected rebased patch
        // addresses; invariant raw-info/addend words remain covered by the
        // masked-image signature above.
        sb.AppendLine("        const auto validateRelocationField = [&](uint32_t patchOffset, uint32_t target, uint8_t type) -> bool {");
        sb.AppendLine($"            if (type == {(byte)RsoRelocationType.R_PPC_NONE}) return true;");
        sb.AppendLine($"            const uint32_t width = type >= {(byte)RsoRelocationType.R_PPC_ADDR16} && type <= {(byte)RsoRelocationType.R_PPC_ADDR16_HA} ? 2u : 4u;");
        sb.AppendLine($"            if (patchOffset > 0x{validationImageSize:X8}u || width > 0x{validationImageSize:X8}u - patchOffset) return false;");
        sb.AppendLine("            const uint32_t patch = base + patchOffset;");
        sb.AppendLine("            const uint32_t read32 = width == 4u ? DynamicModule::ReadImage32(image, patchOffset) : 0u;");
        sb.AppendLine("            const uint16_t read16 = width == 2u ? DynamicModule::ReadImage16(image, patchOffset) : 0u;");
        sb.AppendLine("            switch (type) {");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_NONE}: return true;");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_ADDR32}: return read32 == target;");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_ADDR24}: return (read32 & 0x03FFFFFCu) == (target & 0x03FFFFFCu);");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_ADDR16}:");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_ADDR16_LO}: return read16 == static_cast<uint16_t>(target);");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_ADDR16_HI}: return read16 == static_cast<uint16_t>(target >> 16);");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_ADDR16_HA}: return read16 == static_cast<uint16_t>((target + 0x8000u) >> 16);");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_ADDR14}:");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_ADDR14_BRTAKEN}:");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_ADDR14_BRNTAKEN}: return (read32 & 0x0000FFFCu) == (target & 0x0000FFFCu);");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_REL24}: {{");
        sb.AppendLine("                const int32_t displacement = static_cast<int32_t>(target - patch);");
        sb.AppendLine("                const uint32_t field = displacement < -0x02000000 || displacement > 0x01FFFFFC ? 0x03FFFFFCu : (static_cast<uint32_t>(displacement) & 0x03FFFFFCu);");
        sb.AppendLine("                return (read32 & 0x03FFFFFCu) == field;");
        sb.AppendLine("            }");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_REL14}:");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_REL14_BRTAKEN}:");
        sb.AppendLine($"            case {(byte)RsoRelocationType.R_PPC_REL14_BRNTAKEN}: return (read32 & 0x0000FFFCu) == ((target - patch) & 0x0000FFFCu);");
        sb.AppendLine("            default: return false;");
        sb.AppendLine("            }");
        sb.AppendLine("        };");

        sb.AppendLine("        const auto validateRelocationTable = [&](uint32_t tableOffset, uint32_t count, bool external) -> bool {");
        sb.AppendLine("            for (uint32_t index = 0; index < count; ++index) {");
        sb.AppendLine($"                const uint32_t entryOffset = tableOffset + index * 0x{RsoFile.RelocationEntrySize:X}u;");
        sb.AppendLine("                const uint32_t rawInfo = DynamicModule::ReadImage32(image, entryOffset + 4u);");
        sb.AppendLine("                const uint32_t symbolIndex = rawInfo >> 8;");
        sb.AppendLine("                const uint8_t type = static_cast<uint8_t>(rawInfo);");
        sb.AppendLine("                const uint32_t addend = DynamicModule::ReadImage32(image, entryOffset + 8u);");
        sb.AppendLine("                if (external) {");
        sb.AppendLine($"                    if (symbolIndex >= 0x{file.Imports.Count:X8}u) return false;");
        sb.AppendLine("                } else {");
        sb.AppendLine($"                    if (symbolIndex >= 0x{file.Sections.Count:X8}u) return false;");
        sb.AppendLine("                }");
        sb.AppendLine($"                const uint32_t canonicalPatch = DynamicModule::ReadImage32(kExpected_{SanitizeIdentifier(compilation.TemplateContext.TemplateId)}, entryOffset);");
        sb.AppendLine($"                const uint32_t patchOffset = canonicalPatch >= 0x{compilation.TemplateContext.CanonicalImageBase:X8}u");
        sb.AppendLine($"                    ? canonicalPatch - 0x{compilation.TemplateContext.CanonicalImageBase:X8}u : canonicalPatch;");
        sb.AppendLine($"                if (patchOffset >= 0x{validationImageSize:X8}u) return false;");
        sb.AppendLine("                if (DynamicModule::ReadImage32(image, entryOffset) != base + patchOffset) return false;");
        if (file.Imports.Count != 0)
        {
            sb.AppendLine($"                if (external && kResolvedImports_{SanitizeIdentifier(compilation.TemplateContext.TemplateId)}[symbolIndex] == 0) {{");
            sb.AppendLine("                    continue;");
            sb.AppendLine("                }");
        }
        sb.AppendLine("                uint32_t target = 0;");
        sb.AppendLine("                if (external) {");
        sb.AppendLine($"                    const uint32_t importEntry = 0x{file.Header.ImportTableOffset:X8}u + symbolIndex * 0x{RsoFile.ImportEntrySize:X}u;");
        sb.AppendLine("                    target = DynamicModule::ReadImage32(image, importEntry + 4u) + addend;");
        sb.AppendLine("                } else {");
        sb.AppendLine($"                    const uint32_t sectionEntry = 0x{file.Header.SectionTableOffset:X8}u + symbolIndex * 0x{RsoFile.SectionEntrySize:X}u;");
        sb.AppendLine("                    target = DynamicModule::ReadImage32(image, sectionEntry) + addend;");
        sb.AppendLine("                }");
        sb.AppendLine("                if (!validateRelocationField(patchOffset, target, type)) return false;");
        sb.AppendLine("            }");
        sb.AppendLine("            return true;");
        sb.AppendLine("        };");
        bool TableRetained(uint offset, int count, uint entrySize, string label)
        {
            if (count == 0 || offset >= validationImageSize)
                return false;
            var end = (ulong)offset + (ulong)(uint)count * entrySize;
            if (end > validationImageSize)
                throw new InvalidDataException($"Dynamic RSO {label} table straddles the retained runtime image");
            return true;
        }

        var internalTableRetained = TableRetained(
            file.Header.InternalRelocationTableOffset,
            file.InternalRelocations.Count,
            RsoFile.RelocationEntrySize,
            "internal relocation");
        var externalTableRetained = TableRetained(
            file.Header.ExternalRelocationTableOffset,
            file.ExternalRelocations.Count,
            RsoFile.RelocationEntrySize,
            "external relocation");
        if (externalTableRetained && file.Imports.Count != 0 &&
            !TableRetained(file.Header.ImportTableOffset, file.Imports.Count, RsoFile.ImportEntrySize, "import"))
        {
            throw new InvalidDataException("Dynamic RSO retained external relocation table requires the import table to be retained");
        }

        if (internalTableRetained)
            sb.AppendLine($"        if (!validateRelocationTable(0x{file.Header.InternalRelocationTableOffset:X8}u, 0x{file.InternalRelocations.Count:X8}u, false)) return false;");
        if (externalTableRetained)
            sb.AppendLine($"        if (!validateRelocationTable(0x{file.Header.ExternalRelocationTableOffset:X8}u, 0x{file.ExternalRelocations.Count:X8}u, true)) return false;");
    }

    private static string RuntimeAddressExpression(
        RsoDynamicTemplateCompilation compilation,
        uint canonicalAddress)
    {
        if (canonicalAddress == 0)
            return "0u";

        var imageBase = compilation.TemplateContext.CanonicalImageBase;
        var imageEnd = (ulong)imageBase + compilation.TemplateContext.ImageSize;
        if (canonicalAddress >= imageBase && (ulong)canonicalAddress < imageEnd)
            return $"base + 0x{canonicalAddress - imageBase:X8}u";

        if (compilation.TemplateContext.BssSize != 0)
        {
            var bssBase = compilation.TemplateContext.CanonicalBssBase;
            var bssEnd = (ulong)bssBase + compilation.TemplateContext.BssSize;
            if (canonicalAddress >= bssBase && (ulong)canonicalAddress < bssEnd)
                return $"bss + 0x{canonicalAddress - bssBase:X8}u";
        }

        return $"0x{canonicalAddress:X8}u";
    }

    private static uint RelocationPatchBits(RsoRelocationType type) => type switch
    {
        RsoRelocationType.R_PPC_NONE => 0u,
        RsoRelocationType.R_PPC_ADDR32 => uint.MaxValue,
        RsoRelocationType.R_PPC_ADDR16 or RsoRelocationType.R_PPC_ADDR16_LO or
            RsoRelocationType.R_PPC_ADDR16_HI or RsoRelocationType.R_PPC_ADDR16_HA => 0xFFFFu,
        RsoRelocationType.R_PPC_ADDR24 or RsoRelocationType.R_PPC_REL24 => 0x03FFFFFCu,
        RsoRelocationType.R_PPC_ADDR14 or RsoRelocationType.R_PPC_ADDR14_BRTAKEN or
            RsoRelocationType.R_PPC_ADDR14_BRNTAKEN or RsoRelocationType.R_PPC_REL14 or
            RsoRelocationType.R_PPC_REL14_BRTAKEN or RsoRelocationType.R_PPC_REL14_BRNTAKEN => 0x0000FFFCu,
        _ => throw new NotSupportedException($"Dynamic RSO signature does not support relocation type {(byte)type}"),
    };

    private static int RelocationPatchWidth(RsoRelocationType type) => type switch
    {
        RsoRelocationType.R_PPC_NONE => 0,
        RsoRelocationType.R_PPC_ADDR16 or RsoRelocationType.R_PPC_ADDR16_LO or
            RsoRelocationType.R_PPC_ADDR16_HI or RsoRelocationType.R_PPC_ADDR16_HA => 2,
        RsoRelocationType.R_PPC_ADDR32 or RsoRelocationType.R_PPC_ADDR24 or
            RsoRelocationType.R_PPC_ADDR14 or RsoRelocationType.R_PPC_ADDR14_BRTAKEN or
            RsoRelocationType.R_PPC_ADDR14_BRNTAKEN or RsoRelocationType.R_PPC_REL24 or
            RsoRelocationType.R_PPC_REL14 or RsoRelocationType.R_PPC_REL14_BRTAKEN or
            RsoRelocationType.R_PPC_REL14_BRNTAKEN => 4,
        _ => throw new NotSupportedException($"Dynamic RSO signature does not support relocation type {(byte)type}"),
    };

    private static void AppendByteArray(StringBuilder sb, string name, IReadOnlyList<byte> bytes)
    {
        // A comma-separated 0xNN initializer for multi-megabyte RSO images
        // creates millions of C++ tokens and makes clang spend minutes and
        // gigabytes parsing the registrar. Hex-escaped string chunks encode the
        // exact same byte payload with dramatically fewer tokens. The implicit
        // trailing NUL is harmless because all consumers use the explicit image
        // size rather than sizeof(array).
        sb.AppendLine($"const uint8_t {name}[] =");
        const int bytesPerLiteral = 4096;
        const string hex = "0123456789ABCDEF";
        for (var offset = 0; offset < bytes.Count; offset += bytesPerLiteral)
        {
            sb.Append("    \"");
            var end = Math.Min(offset + bytesPerLiteral, bytes.Count);
            for (var index = offset; index < end; ++index)
            {
                var value = bytes[index];
                sb.Append('\\').Append('x').Append(hex[value >> 4]).Append(hex[value & 15]);
            }
            sb.AppendLine("\"");
        }
        sb.AppendLine("    ;");
    }

    private static void AppendUInt32Array(StringBuilder sb, string name, IReadOnlyList<uint> values)
    {
        sb.AppendLine($"const uint32_t {name}[] = {{");
        for (var offset = 0; offset < values.Count; offset += 8)
        {
            sb.Append("    ");
            var end = Math.Min(offset + 8, values.Count);
            for (var index = offset; index < end; ++index)
            {
                if (index != offset) sb.Append(' ');
                sb.Append($"0x{values[index]:X8}u,");
            }
            sb.AppendLine();
        }
        sb.AppendLine("};");
    }

    private static void AppendByteValueArray(StringBuilder sb, string name, IReadOnlyList<byte> values)
    {
        sb.AppendLine($"const uint8_t {name}[] = {{");
        if (values.Count == 0)
        {
            sb.AppendLine("    0,");
        }
        else
        {
            for (var offset = 0; offset < values.Count; offset += 32)
            {
                sb.Append("    ");
                var end = Math.Min(offset + 32, values.Count);
                for (var index = offset; index < end; ++index)
                {
                    if (index != offset) sb.Append(' ');
                    sb.Append(values[index]);
                    sb.Append(',');
                }
                sb.AppendLine();
            }
        }
        sb.AppendLine("};");
    }

    private static void AppendIncbinArray(StringBuilder sb, string name, string binaryPath)
    {
        var path = Path.GetFullPath(binaryPath).Replace('\\', '/');
        var escapedPath = EscapeString(path);
        sb.AppendLine($"extern \"C\" const uint8_t {name}[];");
        sb.AppendLine("__asm__(");
        sb.AppendLine("#if defined(_WIN32)");
        sb.AppendLine("    \".section .rdata,\\\"dr\\\"\\n\"");
        sb.AppendLine("#elif defined(__APPLE__)");
        sb.AppendLine("    \".section __TEXT,__const\\n\"");
        sb.AppendLine("#else");
        sb.AppendLine("    \".section .rodata\\n\"");
        sb.AppendLine("#endif");
        sb.AppendLine("    \".balign 16\\n\"");
        sb.AppendLine("#if defined(__APPLE__)");
        sb.AppendLine($"    \".globl _{name}\\n\"");
        sb.AppendLine($"    \"_{name}:\\n\"");
        sb.AppendLine("#else");
        sb.AppendLine($"    \".globl {name}\\n\"");
        sb.AppendLine($"    \"{name}:\\n\"");
        sb.AppendLine("#endif");
        sb.AppendLine($"    \".incbin \\\"{escapedPath}\\\"\\n\"");
        sb.AppendLine(");");
    }

    private static string SanitizeIdentifier(string value)
    {
        var sb = new StringBuilder(value.Length + 1);
        if (value.Length == 0 || !(char.IsLetter(value[0]) || value[0] == '_')) sb.Append('_');
        foreach (var ch in value)
            sb.Append(char.IsLetterOrDigit(ch) || ch == '_' ? ch : '_');
        return sb.ToString();
    }

    private static string EscapeString(string value) =>
        value.Replace("\\", "\\\\", StringComparison.Ordinal).Replace("\"", "\\\"", StringComparison.Ordinal);
}
